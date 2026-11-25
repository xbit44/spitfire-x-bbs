#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include <iostream>
namespace win=sfx::platform::win32;namespace ins=win::installation;namespace app=sfx::application;namespace storage=sfx::adapters::storage;
int wmain(int argc,wchar_t** argv)try{
    ins::require(argc==3,"closed rc8 fixture action and root");const auto root=ins::canonical_root(argv[2]);const auto leaf=root.filename().wstring();
    ins::require(root.parent_path()==ins::Path(L"C:\\Program Files")&&(leaf==L"SFX-RC8-Upgrade-Test-20261107-r1"||leaf==L"SFX-RC8-Fresh-Test-20261107-r1"||leaf==L"SFX-RC8-Copy-Test-20261107-r1"),"only new rc8 isolated roots");
    const std::wstring action=argv[1];const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);
    const auto service=ins::read_service_registration(root);ins::require(service&&ins::query_owned_service(*service).state==SERVICE_STOPPED,"exact fixture stopped");win::OfflineInstallationGuard guard;ins::require(guard.acquire(root,owner).succeeded(),"exclusive fixture guard");
    if(action==L"retire"){
        ins::require(ins::elevated(),"normal administrator approval required");SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);ins::require(manager!=nullptr,"SCM connect");SC_HANDLE owned=OpenServiceW(manager,service->name.c_str(),DELETE|SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);ins::require(owned!=nullptr,"exact owned service");const bool removed=DeleteService(owned)!=FALSE;CloseServiceHandle(owned);CloseServiceHandle(manager);ins::require(removed,"retire owned service only");std::cout<<"PASS retired rc8 fixture service; files retained\n";return 0;
    }
    if(action==L"callers2"||action==L"callers1"){
        ins::require(!ins::elevated()&&leaf==L"SFX-RC8-Upgrade-Test-20261107-r1","ordinary named caller fixture only");
        auto config=win::read_operational_configuration_file(root);ins::require(config.configuration.has_value(),"read fixture configuration");
        ins::require(config.configuration->maximum_sessions_per_account==1||config.configuration->maximum_sessions_per_account==2,"bounded original test account-session policy");
        config.configuration->maximum_sessions_per_account=action==L"callers2"?2:1;
        ins::require(win::publish_operational_configuration_file(root,owner,*config.configuration).succeeded(),"publish bounded caller fixture policy");std::cout<<"PASS closed fixture account-session policy\n";return 0;
    }
    ins::require(!ins::elevated()&&action==L"seed","ordinary stopped fixture seed only");
    win::WindowsLocalStoragePathPolicy paths(owner);storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";storage::SqliteStorage db(settings,paths);ins::require(db.start().succeeded(),"fixture database start");const app::MessageConfigurationActor actor{true,{}};win::WindowsSecureIdentifierGenerator ids;
    for(unsigned i=0;i<2;++i){auto c=db.door_configuration(actor);ins::require(c&&c.value&&!app::find_door(*c.value,static_cast<app::DoorId>(90+i)),"new test Door IDs only");app::DoorConfigurationChange change;change.operation=ids.generate();change.expected_revision=c.value->revision;change.create_only=true;change.door.id=static_cast<app::DoorId>(90+i);change.door.name=i?"RC8 loopback RLogin":"RC8 loopback Telgate";change.door.code=i?"RC8RLOGIN":"RC8TELGATE";change.door.enabled=true;change.door.allow_multiple=true;
        if(i){change.door.kind=app::DoorExecutionKind::rlogin;change.door.rlogin.host="127.0.0.1";change.door.rlogin.port=18866;change.door.rlogin.prompt_credential=false;}
        else{change.door.kind=app::DoorExecutionKind::telnet;change.door.telnet={"127.0.0.1",18865,"ansi",2};}
        ins::require(db.change_door_configuration(actor,change).succeeded(),"save isolated remote Door through protected API");c=db.door_configuration(actor);ins::require(c&&c.value,"read new catalog");change={};change.operation=ids.generate();change.expected_revision=c.value->revision;change.create_only=true;change.action=app::DoorConfigurationAction::save_entry;change.entry={0,static_cast<std::uint8_t>(90+i),app::DoorEntryKind::door,static_cast<app::DoorId>(90+i),i?"RC8 RLogin":"RC8 Telgate","Owned loopback qualification"};ins::require(db.change_door_configuration(actor,change).succeeded(),"place isolated Door through API");
    }
    ins::require(db.shutdown().succeeded(),"fixture writer shutdown");std::cout<<"PASS seeded two isolated loopback Doors; prior definitions retained\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
