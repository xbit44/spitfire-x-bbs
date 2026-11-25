#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include <iostream>
namespace win=sfx::platform::win32;namespace ins=win::installation;namespace app=sfx::application;namespace storage=sfx::adapters::storage;
int wmain(int argc,wchar_t** argv)try{
    ins::require(argc==3,"closed Section16 fixture action and root");const auto root=ins::canonical_root(argv[2]);const auto leaf=root.filename().wstring();
    ins::require(root.parent_path()==ins::Path(L"C:\\Program Files")&&(leaf==L"SFX-Portable-Upgrade-Test-20261106-r1"||leaf==L"SFX-Portable-Fresh-Test-20261106-r1"||leaf==L"SFX-Portable-Fresh-Test-20261106-r2"||leaf==L"SFX-Portable-Copy-Desktop-Test-20261106-r1"||leaf==L"SFX-Portable-Copy-Desktop-Test-20261106-r2"||leaf==L"SFX-Portable-Copy-Desktop-Test-20261106-r3"||leaf==L"SFX-Portable-Copy-Service-Test-20261106-r1"||leaf==L"SFX-Portable-Copy-Service-Test-20261106-r2"),"new isolated fixture only");
    const std::wstring action=argv[1];const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);
    const auto service=ins::read_service_registration(root);ins::require(service&&ins::query_owned_service(*service).state==SERVICE_STOPPED,"exact fixture service stopped");win::OfflineInstallationGuard guard;ins::require(guard.acquire(root,owner).succeeded(),"exclusive stopped fixture");
    if(action==L"retire"){
        ins::require(ins::elevated(),"normal Windows approval required");SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);ins::require(manager!=nullptr,"SCM connect");SC_HANDLE owned=OpenServiceW(manager,service->name.c_str(),DELETE|SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);ins::require(owned!=nullptr,"exact owned service");const bool removed=DeleteService(owned)!=FALSE;CloseServiceHandle(owned);CloseServiceHandle(manager);ins::require(removed,"retire only fixture registration");std::cout<<"PASS retired exact Section16 test service; files retained\n";return 0;
    }
    if(action==L"classify-empty"||action==L"classify-persistent"||action==L"classification-control"){
        ins::require(leaf==L"SFX-Portable-Fresh-Test-20261106-r1"&&!std::filesystem::exists(root/L"Data"/L"SFX.DB")&&!std::filesystem::exists(root/L"Config"/L"SFX.conf"),"exact untouched pre-bootstrap fixture");
        const auto recovery=root/L"Backup"/L"Recovery";
        if(action==L"classification-control"){
            ins::require(ins::elevated()&&std::filesystem::is_empty(recovery),"new bounded negative control requires consented empty owned Recovery");
            auto descriptor=ins::structural_descriptor(owner);ins::create_file(recovery/L"section16-history-control.bin",{'H','I','S','T','O','R','Y'},descriptor.get());
            std::cout<<"PASS retained exact synthetic recovery-history control\n";return 0;
        }
        ins::require(!ins::elevated(),"ordinary classification proof");const auto state=win::alpha_installation_has_persistent_state(root,owner);
        ins::require(state&&state.value&&*state.value==(action==L"classify-persistent"),"empty protected layout permits first run; any retained history prevents it");
        std::cout<<"PASS bounded fresh-state classification; no repair or data changes\n";return 0;
    }
    ins::require(!ins::elevated()&&(action==L"seed"||action==L"credentials"),"ordinary fixture data action");
    win::WindowsLocalStoragePathPolicy paths(owner);storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";storage::SqliteStorage db(settings,paths);ins::require(db.start().succeeded(),"fixture writer start");const app::MessageConfigurationActor actor{true,{}};auto config=db.ftn_configuration(actor);ins::require(bool(config)&&config.value.has_value(),"fixture FTN catalog");
    if(action==L"seed"){
        ins::require(leaf==L"SFX-Portable-Upgrade-Test-20261106-r1"&&!std::filesystem::exists(root/L"Config"/L"board.key"),"seed only original rc6 fixture before migration");
        app::FtnNetwork network;network.code="PORTABLE";network.name="Synthetic portable recovery";network.address="2:999/1@portable";network.enabled=false;config.value->networks.push_back(network);
        app::FtnLink link;link.network="PORTABLE";link.code="PEER";link.name="Synthetic disabled peer";link.address="2:999/2@portable";link.host="127.0.0.1";link.enabled=false;
        for(auto item:{std::pair{win::FtnCredentialKind::session,&link.credentials.session},std::pair{win::FtnCredentialKind::packet,&link.credentials.packet},std::pair{win::FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{win::FtnCredentialKind::tic,&link.credentials.tic}}){const auto encrypted=win::protect_legacy_ftn_secret(root.u8string(),link.network,link.code,item.first,"testkey");ins::require(encrypted.has_value(),"ordinary original-context legacy encryption");*item.second=*encrypted;}
        config.value->links.push_back(link);app::Identifier128 operation;operation.bytes.fill(0x74);ins::require(bool(db.change_ftn_configuration(actor,operation.bytes,*config.value)),"save four synthetic legacy credentials without enabling network");
    }else{
        ins::require(!win::PortableCredentialReader(root).legacy_allowed(),"upgraded board has established portable credential material before copy");
        unsigned verified{};for(const auto& link:config.value->links)if(link.network=="PORTABLE"&&link.code=="PEER")for(auto item:{std::pair{win::FtnCredentialKind::session,&link.credentials.session},std::pair{win::FtnCredentialKind::packet,&link.credentials.packet},std::pair{win::FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{win::FtnCredentialKind::tic,&link.credentials.tic}}){auto value=win::unprotect_ftn_secret(root.u8string(),link.network,link.code,item.first,*item.second);ins::require(value&&value->view()=="testkey","actual saved credential decode");if(leaf.find(L"Copy")!=leaf.npos)ins::require(!win::unprotect_legacy_ftn_secret(root.u8string(),link.network,link.code,item.first,*item.second),"copied root cannot use old DPAPI purpose; portable alias must work");++verified;}
        ins::require(verified==4,"all four saved credential families verified");
    }
    ins::require(db.shutdown().succeeded(),"fixture writer stopped");std::cout<<"PASS Section16 synthetic installed credentials; no remote network connection\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
