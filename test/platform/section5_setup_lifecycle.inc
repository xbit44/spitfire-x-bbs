namespace menu_setup_test {
namespace app=sfx::application;
#include "stock_menu_payload.inc"
void run(bool corrections=false, bool followup=false, bool welcome=false, bool polish=false, bool findings=false, bool october=false, bool newsletter=false, bool section6=false, bool section6_polish=false, bool asleep_color=false, bool section7=false, bool section7_corrections=false, bool section7_polish=false, bool section7_text=false, bool section7_followup=false, bool section8a=false, bool section8a_polish=false, bool section8a_corrections=false, bool section8a_login=false, bool section8a_admin=false, bool section8a_config=false, bool presentation=false, bool help=false, bool menu_followup=false) {
    install::require(!install::elevated(),"ordinary Section5 coordinator required");
const install::Path legacy_root(section7 ? L"C:\\xtest-sfx-section7-20261004" : asleep_color ? L"C:\\xtest-sfx-asleep-color-20261004" : section6_polish ? L"C:\\xtest-sfx-section6-polish-20261004" : section6 ? L"C:\\xtest-sfx-section6-final-20261003" : newsletter ? L"C:\\xtest-sfx-newsletter-20261002" : october ? L"C:\\xtest-sfx-october1-final-20261001" : findings ? L"C:\\xtest-sfx-findings-final-20260930" : polish ? L"C:\\xtest-sfx-section5-polish-20260930" : welcome ? L"C:\\xtest-sfx-welcome-placement-20260928" : followup ? L"C:\\xtest-sfx-login-menu-final-20260928" : corrections ? L"C:\\xtest-sfx-menu-fixes-final-20260928" : L"C:\\xtest-sfx-section5-20260927");
    const install::Path root=section8b_test::active?section8b_test::root():
    footer_format_test::active?install::Path(L"C:\\xtest-sfx-section8a-footer-qualified-r4-20261008"):
    menu_followup?install::Path(L"C:\\xtest-sfx-section8a-menu-followup-qualified-r5-20261008"):
    help?install::Path(L"C:\\xtest-sfx-section8a-help-qualified-r2-20261008"):
        presentation?install::Path(L"C:\\xtest-sfx-section8a-presentation-qualified-r4-20261007"):
        section8a_config?install::Path(L"C:\\xtest-sfx-section8a-config-qualified-r1-20261007"):
        section8a_admin?install::Path(L"C:\\xtest-sfx-section8a-admin-qualified-r4-20261007"):
        section8a_login?install::Path(L"C:\\xtest-sfx-section8a-login-qualified-20261007"):
        section8a_corrections?install::Path(L"C:\\xtest-sfx-section8a-corrections-qualified-r2-20261006"):
        section8a_polish?install::Path(L"C:\\xtest-sfx-section8a-polish-qualified-r2-20261006"):
        section8a?install::Path(L"C:\\xtest-sfx-section8a-qualified-r2-20261006"):
        section7_followup?install::Path(L"C:\\xtest-sfx-section7-followup-qualified-20261005"):
        section7_text?install::Path(L"C:\\xtest-sfx-section7-text-shipping-20261005"):
        section7_polish?install::Path(L"C:\\xtest-sfx-section7-polish-20261004"):
        section7_corrections?install::Path(L"C:\\xtest-sfx-section7-corrections-20261004"):legacy_root;
    install::require(!std::filesystem::exists(root),"fresh Section5 target must be absent; never adopt another installation");
    const auto owner=install::operator_sid();
    setup::verify_payload();
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    install::require(SUCCEEDED(com),"COM initialization");
    struct Com {~Com(){CoUninitialize();}} release;
    const auto perform=[&](setup::Operation expected) {
        const auto plan=setup::inspect_plan(root,false);
        install::require(plan.operation==expected,"expected Setup operation");
        install::require(setup::execute(plan,{false,false},nullptr,
            [](const std::wstring& text){std::wcout<<text<<std::endl;},[]{return true;}),"Setup consent cancelled");
        install::verify_runtime_installation(root,owner);
    };
    const auto setup_started=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    perform(setup::Operation::fresh);
    if(section7_text)install::require(install::read_file(root/L"Config"/L"otherbbs.txt",1024).empty(),"fresh Other BBS text starts empty");
    if(section7_followup) {
        const auto list=root/L"Config"/L"otherbbs.txt";
        install::require(!std::filesystem::exists(root/L"conf"),"fresh Setup never creates conf");
        const auto fresh=sfx::platform::win32::alpha_installation_has_persistent_state(root,owner);
        install::require(fresh && fresh.value && !*fresh.value,"protected empty Config lists do not suppress first run");
        {
            auto exclusive=install::open_object(list,false,GENERIC_READ,0);
            const auto blocked=sfx::platform::win32::alpha_installation_has_persistent_state(root,owner);
            install::require(blocked && blocked.value && *blocked.value,"unreadable empty list remains conservative evidence");
        }
        {
            auto file=install::open_object(list,false,GENERIC_WRITE,0);DWORD written{};
            install::require(WriteFile(file.get(),"X",1,&written,nullptr) && written==1 && FlushFileBuffers(file.get()),"nonempty first-run fixture");
        }
        const auto occupied=sfx::platform::win32::alpha_installation_has_persistent_state(root,owner);
        install::require(occupied && occupied.value && *occupied.value,"nonempty owner list does not enable first run");
        {
            auto file=install::open_object(list,false,GENERIC_WRITE,0);
            install::require(SetEndOfFile(file.get()) && FlushFileBuffers(file.get()),"restore exact empty disposable first-run list");
        }
        std::cout<<"PASS first-run empty/nonempty/unreadable Config list classification; no obsolete folder"<<std::endl;
    }
    std::int64_t section7_initialized{};
    if(polish) for(const auto* base:{L"ALL",L"CHATDONE",L"EVENTIME",L"GOODBYE",L"LOCKOUT",L"NEWUSER",L"SFASLEEP",L"SFNWSLTR"})
        for(const auto* extension:{L".ANS",L".ASC"})
            install::require(!std::filesystem::exists(root/L"Display"/(std::wstring(base)+extension)),"Setup must not install optional event files");
    const auto initial=install::read_receipt(root,owner);
    if(findings) {
        const auto discovered=install::registered_installations({initial.id});
        install::require(discovered.size()==1 && discovered.front().available &&
            install::suggested_installation_root(discovered)==root,"actual protected registry discovery selects exact fixture root");
        if(october) {
            const auto duplicates=install::registered_installations({initial.id,initial.id,initial.id,initial.id,initial.id});
            install::require(duplicates.size()==1 && duplicates.front().available && duplicates.front().receipt.id==initial.id &&
                install::suggested_installation_root(duplicates)==root,"five references to one protected installation produce one selected target");
            auto stale=initial;stale.id=L"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
            auto lowercase=initial;lowercase.root=install::Path(root.native()+L"\\");
            for(const auto& records:std::vector<std::vector<install::RegisteredInstallation>>{
                {{stale,false},{initial,true},{lowercase,false}},{{initial,true},{stale,false},{lowercase,false}}}) {
                const auto grouped=install::unique_installation_targets(records);
                install::require(grouped.size()==1 && grouped.front().available && grouped.front().receipt.id==initial.id,
                    "historical duplicate before/after live record cannot replace the verified bound receipt");
            }
            const auto unavailable=install::unique_installation_targets({{stale,false},{lowercase,false}});
            install::require(unavailable.size()==1 && !unavailable.front().available &&
                install::suggested_installation_root(unavailable)==root,"stale-only duplicate never becomes fresh C fallback");
        }
        install::Receipt first;first.root=L"D:\\SFX";
        install::Receipt second;second.root=L"E:\\SFX";
        const std::vector<install::RegisteredInstallation> one{{first,true}},missing{{first,false}},multiple{{first,true},{second,true}};
        install::require(install::suggested_installation_root(one)==first.root && install::suggested_installation_root(missing)==first.root,
            "single D target retained even when unavailable, never C fallback");
        install::require(install::suggested_installation_root({{second,true}})==second.root && install::suggested_installation_root(multiple).empty(),
            "single E target selected; multiple roots require explicit choice");
        install::require(install::recorded_installation_root(missing,L"d:\\sfx") && !install::recorded_installation_root(missing,L"C:\\SFX"),
            "recorded missing root cannot be treated as fresh; explicit other roots remain distinct");
        install::require(install::suggested_installation_root({{first,false},{second,true}})==second.root,
            "one verified current BBS selected over unavailable historical paths");
        std::cout<<"PASS protected installation discovery and single/multiple/unavailable drive selection"<<std::endl;
    }
    std::map<std::wstring,std::string> expected;
    for(const auto& asset:stock_menu_assets) {
        const auto digest=install::file_sha256(root/L"Display"/asset.filename);
        install::require(digest==asset.sha256,"fresh installed menu differs from exact embedded asset");
        expected.emplace(asset.filename,digest);
    }
    install::require(!std::filesystem::exists(root/L"Display/door.ans") &&
        !std::filesystem::exists(root/L"Display/fgrp.ans") && !std::filesystem::exists(root/L"Display/farea.ans"),"excluded stock assets remain absent");
    if(corrections) {
        install::require(expected.size()==23U,"matched menu pairs plus five exact owner bulletin defaults required");
        for(const auto* name:{L"dbul.ans",L"dbul.asc",L"bullet2.ans"})
            install::require(!std::filesystem::exists(root/L"Display"/name),"excluded or unsupplied art must not be installed");
    }
    std::cout<<"PASS fresh installed stock: "<<expected.size()<<" exact file hashes; excluded menus absent"<<std::endl;
    {
        sfx::apps::runtime::AlphaRuntimeHost host(root);
        install::require(host.start().code==app::LocalControlResponseCode::setup_required,"fresh first-run state");
        app::AlphaBootstrapRequest request;
        request.configuration.bbs_name="Section5 Menu Fixture";request.configuration.bbs_time_zone="Pacific Standard Time";
        request.configuration.logical_nodes=1;request.configuration.telnet.bind_address="127.0.0.1";
        request.configuration.telnet.port=25281;request.configuration.telnet.bot_gate_enabled=false;
        request.configuration.botgate.enabled=false;request.configuration.ssh.enabled=followup;
        if(followup) {request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=25282;}
        request.sysop_name=followup?"xbit":"Menu Fixture Sysop";request.sysop_password="Menu-fixture-81a9";request.system_password="Menu-fixture-72f1";
        if(section8b_test::active){
            request.configuration.bbs_name="Section8B Artwork Fixture";request.configuration.logical_nodes=4;
            request.sysop_name="SSH Fixture";request.sysop_password="Sfx-test-password-123";request.system_password="Sfx-system-password-456";
        }
        if(section6) {
            request.configuration.logical_nodes=4;request.configuration.telnet.port=25291;request.configuration.ssh.port=25292;
            request.sysop_name="SSH Fixture";request.sysop_password="Sfx-test-password-123";request.system_password="Sfx-system-password-456";
        }
        if(asleep_color) {
            request.configuration.caller_afk_minutes=2;request.configuration.login_afk_seconds=30;
            request.configuration.menus.text_color=10; // distinct board fallback, not a new product default
        }
        if(presentation)request.configuration.caller_afk_minutes=2;
        install::require(host.bootstrap(request).code==app::LocalControlResponseCode::ok,"fresh supported bootstrap");
        if(section6 || section8b_test::active) for(const auto* name:{"Caller One","Caller Two","Caller Three","Caller Four"}) {
            app::LocalAccountRequest create;create.action=app::LocalAccountAction::create_user;create.name=name;create.expected_revision=1;
            create.security_level=10;create.purge_allowed=true;create.password=create.confirmation="Sfx-test-password-123";
            install::require(host.administer_account(create).code==app::LocalAccountResultCode::ok,"Section6 supported account seeding");
        }
        if(section7) {
            const auto opening=host.classic_information_snapshot(0);
            install::require(opening.code==app::LocalAccountResultCode::ok && !opening.opening.configured_date && opening.opening.initialization_utc>=setup_started,"fresh opening date uses this actual Setup initialization");
            section7_initialized=opening.opening.initialization_utc;
            app::ClassicInformationChange change;change.action=app::ClassicInformationAction::opening_date;change.expected_revision=opening.opening.revision;change.date="1984-05-12";
            install::require(host.change_classic_information(change).committed,"historical opening date through live host boundary");
            change={};change.values={"Lifecycle Board","lifecycle.example:2323",""};
            install::require(host.change_classic_information(change).committed,"live catalog seed before update");
        }
        install::require(host.shutdown().code==app::LocalControlResponseCode::ok,"stop before program replacement");
    }
    if(section8a || section8b_test::active) {
        // The parent qualifies the receipt-verified shipping Runtime at generation
        // one, then stops it before genuine Setup maintenance takes its backup.
        std::cout<<"READY 8A fresh"<<std::endl;
        std::string continuation;std::getline(std::cin,continuation);
        install::require(continuation=="continue","fresh installed message qualification must finish before maintenance");
    }
    // Only this just-created, receipt-verified fixture is edited. The original
    // stock bytes stay in the embedded payload and Setup's subsequent backup.
    if(corrections) for(const auto* name:{L"bulletin.asc",L"dbul.ans"}) {
        // Bulletin menu is now stock; dbul remains excluded. Simulate both an
        // independently customized stock file and a preexisting nonstock file.
        const std::string custom="Sysop dynamic area artwork; preserve existing file.\r\n";
        const bool stock=expected.count(name)!=0;
        if(stock)install::require(expected.at(name)==install::file_sha256(root/L"Display"/name),"exact disposable stock before customization");
        install::Handle held(CreateFileW((root/L"Display"/name).c_str(),GENERIC_WRITE,0,nullptr,stock?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        install::require(held.get()!=INVALID_HANDLE_VALUE,"create owned nonstock fixture file");
        if(stock){LARGE_INTEGER zero{};install::require(SetFilePointerEx(held.get(),zero,nullptr,FILE_BEGIN)&&SetEndOfFile(held.get()),"truncate owned stock fixture artwork");}
        DWORD written{};install::require(WriteFile(held.get(),custom.data(),static_cast<DWORD>(custom.size()),&written,nullptr)&&written==custom.size()&&FlushFileBuffers(held.get()),"write nonstock fixture artwork");
        held.reset();expected[name]=install::file_sha256(root/L"Display"/name);
    }
    std::vector<const wchar_t*> custom_names{L"main.ans",L"file.asc"};
    if(section7_text){custom_names.push_back(L"bullet1.ans");custom_names.push_back(L"bullet1.asc");}
    if(section8b_test::followup)for(const auto* name:{L"bulletin.ans",L"bullet1.ans",L"bullet1.asc",L"bullet2.asc"})custom_names.push_back(name);
    for(const auto* name:custom_names) {
        const std::string custom="Owner custom menu; never replace this file.\r\n";
        auto held=install::open_object(root/L"Display"/name,false,GENERIC_WRITE,0);
        LARGE_INTEGER zero{};install::require(SetFilePointerEx(held.get(),zero,nullptr,FILE_BEGIN)&&SetEndOfFile(held.get()),"truncate owned fixture menu");
        DWORD written{};install::require(WriteFile(held.get(),custom.data(),static_cast<DWORD>(custom.size()),&written,nullptr)&&written==custom.size()&&FlushFileBuffers(held.get()),"write fixture custom menu");
        held.reset();expected[name]=install::file_sha256(root/L"Display"/name);
    }
    if(polish) for(const auto* name:{L"CHATDONE.ANS",L"EVENTIME.ASC"}) {
        const std::string custom="Optional Sysop event artwork; preserve existing file.\r\n";
        install::Handle held(CreateFileW((root/L"Display"/name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        install::require(held.get()!=INVALID_HANDLE_VALUE,"create optional Sysop fixture artwork");
        DWORD written{};install::require(WriteFile(held.get(),custom.data(),static_cast<DWORD>(custom.size()),&written,nullptr)&&written==custom.size()&&FlushFileBuffers(held.get()),"write optional Sysop fixture artwork");
        held.reset();expected[name]=install::file_sha256(root/L"Display"/name);
    }
    if(presentation) {
        // The caller-wire test intentionally installs this optional test art
        // AFTER fresh Setup. Maintenance must preserve it, not assert absence.
        install::require(!install::read_file(root/L"Display"/L"SFASLEEP.ASC",4096).empty(),"presentation AFK fixture artwork exists");
        expected[L"SFASLEEP.ASC"]=install::file_sha256(root/L"Display"/L"SFASLEEP.ASC");
    }
    for(const auto* name:{L"main.asc",L"file.ans",L"msg.ans",L"chat.asc"}) {
        install::require(expected.at(name)==install::file_sha256(root/L"Display"/name),"exact stock before intentional fixture removal");
        install::require(DeleteFileW((root/L"Display"/name).c_str()),"remove exact disposable stock file");
    }
    if(section8b_test::followup){
        // Removing only the ANSI companion must seed it independently while
        // the existing custom ASC remains byte-exact through normal update.
        const auto found=std::find_if(std::begin(stock_menu_assets),std::end(stock_menu_assets),[](const auto& asset){return std::wstring(asset.filename)==L"bulletin.ans";});
        install::require(found!=std::end(stock_menu_assets),"new bulletin ANSI default embedded");
        expected[L"bulletin.ans"]=found->sha256;
        install::require(DeleteFileW((root/L"Display/bulletin.ans").c_str()),"remove only disposable bulletin ANSI fixture companion");
    }
    if(section7_followup) {
        const auto descriptor=install::mutable_descriptor(owner);
        request_followup_legacy_folder();
        install::require(MoveFileExW((root/L"Config"/L"otherbbs.txt").c_str(),(root/L"conf"/L"otherbbs.txt").c_str(),MOVEFILE_WRITE_THROUGH),"legacy fixture move");
        install::create_directory(root/L"conf"/L"extra",descriptor.get());
        install::create_file(root/L"conf"/L"extra"/L"discard.txt",{'O','L','D'},descriptor.get());
    }
    perform(setup::Operation::maintenance);
    if(section7_followup)install::require(!std::filesystem::exists(root/L"conf") && !install::read_file(root/L"Config"/L"otherbbs.txt",4096).empty(),"legacy list moved and entire obsolete folder removed");
    auto updated=install::read_receipt(root,owner);
    install::require(updated.id==initial.id && updated.generation==initial.generation+1,"same installation updated once");
    for(const auto& item:expected) install::require(install::file_sha256(root/L"Display"/item.first)==item.second,"update changed existing file or failed missing-file restoration");
    std::cout<<"PASS update: customized ANSI/ASCII and nonstock files preserved independently; missing stock companions restored; all "<<expected.size()<<" hashes exact"<<std::endl;
    std::map<std::wstring,std::string> backups;
    const auto upgrade_backups=section8b_test::backup_destination?root/L"Backup"/L"Upgrades":root/L"Backup";
    for(const auto& file:std::filesystem::directory_iterator(upgrade_backups)) if(file.path().extension()==L".zip")
        backups.emplace(file.path().filename().wstring(),install::file_sha256(file.path()));
    install::require(!backups.empty(),"genuine maintenance preserved recovery backup");
    // Setup intentionally never overwrites a recovery name. Separate the two
    // automated operations by more than its one-second filename resolution.
    if(followup) Sleep(1200);
    install::require(DeleteFileW((root/L"Display/batchd.ans").c_str()) &&
        DeleteFileW((root/L"Display/batchu.asc").c_str()),"remove exact repair fixture stock");
    if(section8b_test::followup){
        const auto found=std::find_if(std::begin(stock_menu_assets),std::end(stock_menu_assets),[](const auto& asset){return std::wstring(asset.filename)==L"bullet1.asc";});
        install::require(found!=std::end(stock_menu_assets),"owner bulletin ASC default embedded");
        expected[L"bullet1.asc"]=found->sha256;
        install::require(DeleteFileW((root/L"Display/bullet1.asc").c_str()),"remove only disposable bulletin1 ASC fixture companion");
    }
    perform(setup::Operation::maintenance); // same matched media: normal repair
    const auto repaired=install::read_receipt(root,owner);
    install::require(repaired.id==initial.id && repaired.generation==initial.generation+2,"same-media repair advances exactly once");
    for(const auto& item:expected) install::require(install::file_sha256(root/L"Display"/item.first)==item.second,"repair preserved/restored each filename independently");
    for(const auto& item:backups) install::require(install::file_sha256(upgrade_backups/item.first)==item.second,"prior recovery archive changed");
    if(corrections) install::require(!std::filesystem::exists(root/L"Display/dbul.asc"),"update/repair must not restore excluded stock files");
    if(polish) for(const auto* base:{L"ALL",L"GOODBYE",L"LOCKOUT",L"NEWUSER",L"SFASLEEP",L"SFNWSLTR"})
        for(const auto* extension:{L".ANS",L".ASC"}) {
            if(presentation && std::wstring(base)==L"SFASLEEP" && std::wstring(extension)==L".ASC")continue;
            install::require(!std::filesystem::exists(root/L"Display"/(std::wstring(base)+extension)),"maintenance must not restore missing optional event files");
        }
    {
        sfx::apps::runtime::AlphaRuntimeHost host(root);
        install::require(host.start().code==app::LocalControlResponseCode::ok,"repaired installation starts with migrated schema/config");
    if(section7) {
            const auto snapshot=host.classic_information_snapshot(0);
            install::require(snapshot.code==app::LocalAccountResultCode::ok && snapshot.opening.initialization_utc==section7_initialized && snapshot.opening.configured_date=="1984-05-12" && snapshot.boards.entries.size()==1 && snapshot.boards.entries.front().values.name=="Lifecycle Board","update/repair preserves catalog and configured/initial opening dates");
        }
        install::require(host.shutdown().code==app::LocalControlResponseCode::ok,"final installed Runtime stopped");
    }
    std::wcout<<L"PASS Section5 exact installed fresh/update/repair; root="<<root.native()<<L" id="<<initial.id
        <<L" generation="<<repaired.generation<<L" stopped; recovery retained"<<std::endl;
}
}
