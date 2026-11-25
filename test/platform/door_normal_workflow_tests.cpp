// Early integrated path: actual editor controls -> actual SQLite -> normal
// Door menu controller -> owned native child -> same category. Authentication
// and installed-process IPC are intentionally not claimed by this fixture.
#define main retained_workflow_early_main
#define render retained_workflow_early_render
#include "door_command_line_early.cpp"
#undef render
#undef main
#include "door_native_render.hpp"
#include <new>
#include "alpha_local_door.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include "sfx/application/door_session.hpp"
namespace storage=sfx::adapters::storage;
namespace internal=storage::internal;
namespace runtime=sfx::apps::runtime;
namespace {
void pump_door_review(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessage(&message);}GdiFlush();}
void select_door_tab(State& s,unsigned tab){TabCtrl_SetCurSel(s.doors.tabs,tab);NMHDR header{s.doors.tabs,IDC_DOOR_TABS,TCN_SELCHANGE};check(door_configuration_notify(s,&header).has_value(),"actual tab selection");pump_door_review();}
struct ReviewKey {std::wstring path;~ReviewKey(){RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());}};
storage::SqliteStorage* workflow_database{};
app::Identifier128 workflow_id(unsigned n){app::Identifier128 value;value.bytes[0]=25;value.bytes[15]=static_cast<std::uint8_t>(n);return value;}
std::optional<app::DoorCatalog> workflow_snapshot(){const auto result=workflow_database->door_configuration({true,{}});return result?result.value:std::nullopt;}
app::LocalDoorResult workflow_change(const app::DoorConfigurationChange& change){
    const auto decoded=app::decode_door_configuration_change(app::encode_door_configuration_change(change));check(decoded.has_value(),"actual current request codec");
    const auto result=workflow_database->change_door_configuration({true,{}},*decoded);
    return result?app::LocalDoorResult{app::LocalAccountResultCode::ok,*result.value}:app::LocalDoorResult{app::LocalAccountResultCode::policy_rejected};
}
void workflow_seed(storage::SqliteStorage& database,const std::filesystem::path& file){
    check(database.shutdown().succeeded(),"stop empty database before synthetic authentication fixture");
    {internal::SqliteConnection db(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        db.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Fixture Caller','fixture caller',999,'active',1,1,1792248000,1792248000); INSERT INTO sfx_account_statistics(account_id) VALUES(1); INSERT INTO sfx_account_profiles(account_id) VALUES(1); INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1792248000,NULL FROM sfx_accounts; UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash; UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1792248000","owned synthetic caller only");
        internal::SqliteStatement q(db.get(),"INSERT INTO sfx_account_sessions VALUES(?1,1,1,1792248000,0,1792248000,NULL,NULL,?1)","synthetic session is not login evidence");q.bind(1,workflow_id(1));q.step();}
    check(database.start().succeeded(),"start seeded actual storage");
    const auto stored_catalog=database.door_configuration({true,{}});check(stored_catalog&&stored_catalog.value,"authoritative empty Door catalog");
    app::DoorConfigurationChange change;change.operation=workflow_id(2);change.expected_revision=stored_catalog.value->revision;change.action=app::DoorConfigurationAction::save_menu;change.create_only=true;change.menu={1,"Local Games"};
    auto result=database.change_door_configuration({true,{}},change);check(result.succeeded(),"create category through storage authority");
    change={};change.operation=workflow_id(3);change.expected_revision=result.value->revision;change.action=app::DoorConfigurationAction::save_entry;change.create_only=true;change.entry={0,1,app::DoorEntryKind::submenu,1,"Local Games",""};check(database.change_door_configuration({true,{}},change).succeeded(),"place category through storage authority");
}
}
int main(int argc,char** argv){try{
    check(argc==3,"fresh integrated evidence root and exact native fixture");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve earlier integrated evidence");std::filesystem::create_directories(root/"Data");std::filesystem::create_directories(root/"Games");
    std::filesystem::copy_file(argv[2],root/"Games"/"fixture.exe");storage::SqliteStorageSettings options;options.data_directory=root/"Data";options.utc_now=[]{return std::int64_t{1792248000};};sfx::test::FakeStoragePathPolicy path_policy;storage::SqliteStorage database(options,path_policy);check(database.start().succeeded()&&database.health().schema_version==18,"fresh schema18 actual database");workflow_seed(database,root/"Data"/"SFX.DB");workflow_database=&database;
    State s;s.dpi=96;s.instance=GetModuleHandleW(nullptr);s.root=root;const auto key=L"Software\\Spitfire-X BBS\\Tests\\NormalDoorWorkflow\\"+root.filename().wstring();ReviewKey owned_key{key};HKEY h{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&h,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"fresh private theme key");const DWORD theme=1;RegSetValueExW(h,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&theme),sizeof(theme));RegCloseKey(h);s.appearance.~DesktopAppearance();new(&s.appearance)win32::DesktopAppearance(key);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native classes");s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"SFX normal Door integrated review",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1080,860,nullptr,nullptr,s.instance,nullptr);check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"actual editor controls");
    door_catalog_fixture=workflow_snapshot;door_change_fixture=workflow_change;show_page(s,26);refresh_door_configuration_page(s);remember_page(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);layout(s,1080,860);auto& d=s.doors;d.menu=1;door_fill_lists(s);door_show_view(s,DoorPage::entries);remember_page(s);press(s,IDC_DOOR_NEW);
    const auto door_id=d.door;const auto entry_number=d.entry;check(d.view==DoorPage::settings,"normal Add Door slot saved in actual SQLite");door_set(d.name,"Native test program");door_configuration_command(s,IDC_DOOR_NAME,EN_KILLFOCUS);check(door_text(d.code)=="NATIVE_TEST_PROG","name suggests unique code");door_set(d.code,"NATIVE_TEST");door_set(d.startup,(root/"Games").u8string());door_set(d.command_line,"fixture.exe --stdio");SendMessageW(d.program_type,CB_SETCURSEL,1,0);door_configuration_command(s,IDC_DOOR_PROGRAM_TYPE,CBN_SELCHANGE);SendMessageW(d.drop_file,CB_SETCURSEL,0,0);
    select_door_tab(s,1);door_check(d.multiple,false);select_door_tab(s,2);door_set(d.cleanup,"fixture.exe --cleanup \"%N/CLEANED.TXT\"");select_door_tab(s,3);door_set(d.entry_title,"Play native test program");check(save_door_configuration_page(s),"actual editor saves local fields and placement through SQLite");
    check(database.shutdown().succeeded()&&database.start().succeeded(),"saved editor state survives database restart");d.catalog=workflow_snapshot();door_fill_lists(s);select_list(d.entries,entry_number);press(s,IDC_DOOR_EDIT);check(d.door==door_id&&door_text(d.command_line)=="fixture.exe --stdio","reopen normal saved program");pump_door_review();layout(s,1080,860);render(s,root/"01-saved-native-program.bmp");
    const auto owner=win32::installation::operator_sid();runtime::ensure_door_node_directories(root,owner,1);app::DoorReservations reservations;const app::MessageActor actor{workflow_id(1),1,1,1};std::string output;std::vector<std::string> paths;unsigned launches{},cursor{};const std::vector<app::MenuInput> choices{{app::MenuInputKind::number,1},{app::MenuInputKind::number,entry_number},{app::MenuInputKind::number,entry_number},{app::MenuInputKind::command,'Q'}};
    app::MenuSessionPorts menu;menu.snapshot=[]()->std::optional<app::MenuSessionSnapshot>{app::MenuSessionSnapshot snap;snap.access={999,true,1};snap.configuration_revision=1;snap.graphics=app::GraphicsMode::ascii;return snap;};menu.write=[&](std::string_view bytes){output+=bytes;return true;};menu.display=[](const app::TerminalDisplayProgram&){return true;};menu.artwork=[&](const app::TrustedDisplaySelectionRequest& request){if(request.identity!=app::TrustedDisplayIdentity::door_prelaunch)paths.push_back(request.context.door_menu.value_or(""));return app::TrustedDisplaySelectionResult{};};menu.read=[&](app::MenuInputDecoder&,const std::function<bool()>&)->std::optional<app::MenuInput>{check(cursor<choices.size(),"bounded ordinary Door menu choices");return choices[cursor++];};
    app::DoorSessionPorts ports{database,menu,[&]()->std::optional<app::MessageActor>{return actor;},[&](const app::DoorLaunch& launch)->std::optional<app::DoorEnd>{
        const auto fresh=database.door_launch(actor,{launch.entry.menu,launch.entry.number,launch.catalog_revision});check(fresh&&fresh.value,"authoritative selection rechecked before launch");auto lease=reservations.acquire(launch.door.id,launch.door.allow_multiple);check(bool(lease)&&!reservations.acquire(launch.door.id,true),"exclusive shared reservation held across child cleanup");
        const auto started=GetTickCount64();std::string child_output;bool sent{};app::DoorChannel channel;channel.read=[&](std::size_t){if(sent||child_output.find("SFX LOCAL READY")==std::string::npos)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};sent=true;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,{'q'}};};channel.write=[&](std::string_view bytes){child_output+=bytes;output+=bytes;return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,bytes.size()};};channel.stop=[&]()->std::optional<app::DoorEnd>{if(GetTickCount64()-started>10000)return app::DoorEnd::calling_time;return {};};
        const auto result=runtime::run_alpha_local_door(root,owner,fresh.value->door,{1,1,999,"Fixture Caller","Fixture BBS",600,true},channel);check(result.end==app::DoorEnd::returned&&result.cleanup_complete&&!result.cleanup_command_failed&&child_output.find("SFX LOCAL RETURN")!=std::string::npos,"actual native child launch, I/O and cleanup completed");lease->release_after_cleanup();++launches;return result.end;}};
    const auto returned=app::run_door_session(ports);check(returned&&*returned==app::DoorMenuExit::main_menu&&launches==2,"normal menu launches same saved Door twice and returns to BBS");check(paths.size()==4&&paths[1]=="Door Menu > Local Games"&&paths[2]==paths[1]&&paths[3]==paths[1],"each child returns to exact launching category");check(std::filesystem::exists(root/"Node"/"1"/"CLEANED.TXT"),"normal cleanup command uses actual node directory");
    {std::ofstream transcript(root/"02-synthetic-caller-menu.txt",std::ios::binary);transcript<<output;check(transcript.good(),"synthetic-only caller walkthrough retained");}
    check(database.shutdown().succeeded(),"actual storage closed normally");DestroyWindow(s.window);DeleteObject(s.ui_font);DeleteObject(s.heading_font);door_catalog_fixture=nullptr;door_change_fixture=nullptr;workflow_database=nullptr;
    std::cout<<"PASS "<<checks<<" integrated editor/SQLite/menu/native child checks; launches="<<launches<<"; authentication and installed IPC not exercised\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
