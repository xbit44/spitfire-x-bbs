// Test-only coordinator using the unchanged production Setup engine and worker.
// Fixed disposable roots only; alternate-volume lifecycle is serial guarded.
// No shortcuts, manual structural writes, or trust bypass.
#include "setup_engine.hpp"
#include <objbase.h>
#include <shellapi.h>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <algorithm>
#include <aclapi.h>
#include <sddl.h>
#include "../../src/apps/runtime/alpha_runtime_host.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"

namespace setup = sfx::apps::setup;
namespace install = sfx::platform::win32::installation;
namespace footer_format_test { bool active=false; }
namespace section8b_test {
bool backup_redesign=false;
bool backup_two_folder=false;
bool backup_destination=false;
bool active=false;bool fault=false;bool followup=false;bool stock_only=false;bool integration_fault=false;bool ssh_followup=false;bool deadline_followup=false;bool policy_followup=false;bool client_compat=false;bool picker_diagnosis=false;bool empty_batch=false;
install::Path root(){
    if(backup_destination)return install::Path(L"C:\\xtest-sfx-backup-destination-r2-20261011");
    if(backup_two_folder)return install::Path(L"C:\\xtest-sfx-backup-two-folder-r2-20261011");
    if(backup_redesign)return install::Path(L"C:\\xtest-sfx-backup-redesign-r3-20261010");
    if(empty_batch)return install::Path(integration_fault?L"C:\\xtest-sfx-section8b-empty-batch-fault-r1-20261009":L"C:\\xtest-sfx-section8b-empty-batch-qualified-r2-20261009");
    if(picker_diagnosis)return install::Path(L"C:\\xtest-sfx-section8b-netrunner-picker-diagnosis-r1-20261009");
    if(client_compat)return install::Path(integration_fault?L"C:\\xtest-sfx-section8b-client-compat-fault-r1-20261009":L"C:\\xtest-sfx-section8b-client-compat-qualified-r1-20261009");
    if(policy_followup)return install::Path(integration_fault?L"C:\\xtest-sfx-section8b-policy-fault-r1-20261009":L"C:\\xtest-sfx-section8b-policy-qualified-r1-20261009");
    if(deadline_followup)return install::Path(integration_fault?L"C:\\xtest-sfx-section8b-followup-deadline-fault-r1-20261009":L"C:\\xtest-sfx-section8b-followup-deadline-qualified-r1-20261009");
    if(ssh_followup)return install::Path(integration_fault?L"C:\\xtest-sfx-section8b-followup-ssh-fault-r1-20261009":L"C:\\xtest-sfx-section8b-followup-ssh-qualified-r1-20261009");
    if(integration_fault)return install::Path(L"C:\\xtest-sfx-section8b-followup-fault-r2-20261009");
    if(stock_only)return install::Path(L"C:\\xtest-sfx-section8b-followup-stock-r1-20261009");
    if(followup)return install::Path(fault?L"C:\\xtest-sfx-section8b-followup-probe-r1-20261009":L"C:\\xtest-sfx-section8b-followup-qualified-r4-20261009");
    return install::Path(fault?L"C:\\xtest-sfx-section8b-simple-fault-r1-20261008":L"C:\\xtest-sfx-section8b-simple-qualified-r1-20261008");
}
}
// Test-only recreation of a legacy folder. Never relax the protected root ACL.
void create_followup_legacy_folder() {
    const install::Path root(L"C:\\xtest-sfx-section7-followup-qualified-20261005");
    install::require(install::elevated(),"legacy fixture helper requires Windows consent");
    const auto owner=install::operator_sid();
    const auto receipt=install::read_receipt(root,owner);
    install::require(receipt.root==root && receipt.generation==1,"exact fresh legacy fixture receipt required");
    install::verify_structure(receipt);
    auto pin=install::open_object(root,true);
    install::require(!std::filesystem::exists(root/L"conf"),"legacy fixture folder must be absent");
    const auto descriptor=install::mutable_descriptor(owner);
    install::create_directory(root/L"conf",descriptor.get());
}
void request_followup_legacy_folder() {
    wchar_t program[32768]{};
    install::require(GetModuleFileNameW(nullptr,program,32768)>0,"fixture coordinator path");
    SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);
    launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.lpVerb=L"runas";
    launch.lpFile=program;launch.lpParameters=L"--section7-followup-create-conf";launch.nShow=SW_HIDE;
    install::require(ShellExecuteExW(&launch) && launch.hProcess,"legacy fixture Windows consent cancelled");
    install::Handle process(launch.hProcess);DWORD code{};
    install::require(WaitForSingleObject(process.get(),60000)==WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.get(),&code) && code==0,"legacy fixture helper failed");
}
#include "section5_setup_lifecycle.inc"
#include "section5_installed_runtime.inc"
void section8a_offline() {
    namespace app=sfx::application;namespace win=sfx::platform::win32;
    const install::Path root(L"C:\\xtest-sfx-section8a-qualified-r2-20261006");
    const auto owner=install::operator_sid();install::verify_runtime_installation(root,owner);
    install::require(install::read_receipt(root,owner).generation==3,"exact stopped Section8A fixture required");
    app::LocalMessageConfigurationQuery query;
    const auto before=win::offline_message_configuration_snapshot(root,owner,query);
    install::require(before.code==app::LocalAccountResultCode::ok && before.catalog.system_sysop==1 && before.catalog.line_limits.size()==2,"stopped protected-owner message snapshot");
    app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::line_limit;c.operation.bytes[0]=0x8a;c.operation.bytes[14]=0xf0;c.operation.bytes[15]=1;
    c.expected_configuration_revision=before.catalog.revision;c.security_level=10;c.maximum_lines=201;
    c.expected_limit_revision=std::find_if(before.catalog.line_limits.begin(),before.catalog.line_limits.end(),[](const auto& l){return l.security_level==10;})->revision;
    const auto saved=win::offline_change_message_configuration(root,owner,c);
    const auto replay=win::offline_change_message_configuration(root,owner,c);
    install::require(saved.code==app::LocalAccountResultCode::ok && replay.code==app::LocalAccountResultCode::ok && replay.result.replayed && saved.result.configuration_revision==replay.result.configuration_revision,"stopped owner apply/reconciliation one receipt");
    const auto current=win::offline_message_configuration_snapshot(root,owner,query);
    c.operation.bytes[15]=2;c.expected_configuration_revision=current.catalog.revision;c.maximum_lines=200;
    c.expected_limit_revision=std::find_if(current.catalog.line_limits.begin(),current.catalog.line_limits.end(),[](const auto& l){return l.security_level==10;})->revision;
    install::require(win::offline_change_message_configuration(root,owner,c).code==app::LocalAccountResultCode::ok,"restore finite allowance through exclusive stopped-owner port");
    std::cout<<"PASS Section8A protected stopped-owner snapshot/apply/replay/restored allowance"<<std::endl;
}
void alternate_drive(const std::wstring& action) {
    install::require(!install::elevated(),"ordinary alternate-drive coordinator required");
    wchar_t label[64]{}, fs[32]{}; DWORD serial = 0;
    install::require(GetVolumeInformationW(L"R:\\",label,64,&serial,nullptr,nullptr,fs,32) &&
        std::wstring(label)==L"SFX-ALT-0927" && std::wstring(fs)==L"NTFS" && serial==0x205e0bb2,
        "exact disposable VHD required");
    const install::Path root(L"R:\\SFX");
    const bool fresh=action==L"--alternate-fresh", uninstall=action==L"--alternate-uninstall";
    const bool start_only=action==L"--alternate-start";
    auto anchors=install::protect_ancestors(root,install::process_token().get());
    const auto root_security = [&] {
        PSECURITY_DESCRIPTOR raw = nullptr;
        install::require(GetSecurityInfo(anchors.front().get(),SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&raw)==ERROR_SUCCESS,
            "read root security");
        install::Descriptor sd(raw); wchar_t* text=nullptr;
        install::require(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.get(),SDDL_REVISION_1,
            OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&text,nullptr),"describe root security");
        install::Descriptor release(text); return std::wstring(text);
    };
    const auto before = root_security();
    if(fresh) install::require(!std::filesystem::exists(root),"fresh test root must be absent");
    else install::verify_runtime_installation(root,install::operator_sid());
    if (!start_only) {
    const auto plan=setup::inspect_plan(root,uninstall);
    install::require(plan.operation==(fresh ? setup::Operation::fresh : uninstall ? setup::Operation::uninstall :
        setup::Operation::maintenance),"expected genuine Setup lifecycle plan");
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    install::require(SUCCEEDED(com),"COM initialization");
    const bool done=setup::execute(plan,{false,false},nullptr,
        [](const std::wstring& text){std::wcout<<text<<std::endl;},[]{return true;});
    CoUninitialize(); install::require(done,"Setup consent cancelled");
    }
    install::require(before==root_security(),"Setup must not alter test volume ACL");
    const auto sentinel = root/L"Files"/L"alternate-drive-preserve.txt";
    const std::vector<std::uint8_t> marker{'S','F','X','-','D','R','I','V','E','-','T','E','S','T'};
    if(fresh) {
        auto sd=install::mutable_descriptor(install::operator_sid());
        install::create_file(sentinel,marker,sd.get());
    }
    install::require(install::read_file(sentinel,100)==marker,"mutable data preserved");
    if(!uninstall) {
        install::verify_runtime_installation(root,install::operator_sid());
        const auto receipt=install::read_receipt(root,install::operator_sid());
        std::wcout<<L"INSTALLED identity="<<receipt.id<<L" generation="<<receipt.generation<<std::endl;
        for (const auto& path : {root, root/L"Bin", root/L"Bin"/L"SFXRuntime.exe"}) {
            for (const DWORD access : {DWORD(WRITE_DAC), DWORD(WRITE_OWNER), DWORD(DELETE), DWORD(FILE_WRITE_DATA)}) {
                HANDLE writable=CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(writable!=INVALID_HANDLE_VALUE) CloseHandle(writable);
                install::require(writable==INVALID_HANDLE_VALUE && GetLastError()==ERROR_ACCESS_DENIED,
                    "ordinary token cannot replace or take over protected SFX objects");
            }
        }
        sfx::apps::runtime::AlphaRuntimeHost host(root);
        const auto start=host.start();
        std::cout<<"Runtime start: "<<start.diagnostic_code<<std::endl;
        if (fresh || start_only) {
            install::require(start.code==sfx::application::LocalControlResponseCode::setup_required,
                "genuine fresh installed Runtime entry reaches first-run setup");
            sfx::application::AlphaBootstrapRequest request;
            request.configuration.bbs_name="Alternate Drive Fixture";
            request.configuration.bbs_time_zone="Pacific Standard Time";
            request.configuration.logical_nodes=1;
            request.configuration.telnet.enabled=true;
            request.configuration.telnet.bind_address="127.0.0.1";
            request.configuration.telnet.port=25279;
            request.configuration.telnet.bot_gate_enabled=false;
            request.configuration.botgate.enabled=false;
            request.configuration.ssh.enabled=false;
            request.sysop_name="Drive Fixture Sysop";
            request.sysop_password="Drive-fixture-81a9";
            request.system_password="Drive-fixture-72f1";
            const auto boot=host.bootstrap(request);
            std::cout<<"Runtime bootstrap: "<<boot.diagnostic_code<<std::endl;
            install::require(boot.code==sfx::application::LocalControlResponseCode::ok && host.status().telnet_active,
                "first-run configuration and local listener active");
        } else install::require(start.code==sfx::application::LocalControlResponseCode::ok && host.status().telnet_active,
            "updated installation starts existing BBS");
        install::require(host.shutdown().code==sfx::application::LocalControlResponseCode::ok,
            "installed Runtime host shuts down cleanly");
    } else {
        for(const auto* program:install::programs) install::require(!std::filesystem::exists(root/L"Bin"/program),
            "uninstall removes only verified programs");
        bool refused=false; try{ (void)install::read_receipt(root,install::operator_sid()); }
        catch(const std::exception&){refused=true;}
        install::require(refused,"uninstall removes current receipt");
    }
    std::wcout<<L"ALTERNATE SETUP PASS "<<action<<L" root ACL unchanged; data preserved"<<std::endl;
}
int wmain(int argc, wchar_t** argv) {
    // Reuse the maintenance workflow, never its old installation root.
    std::wstring policy_argument;
    if(argc==2){
        const std::wstring destination_value=argv[1],destination_prefix=L"--backup-destination-";
        if(destination_value.find(destination_prefix)==0) {
            const auto action=destination_value.substr(destination_prefix.size());
            install::require(action==L"lifecycle" || action==L"fresh-runtime" || action==L"runtime", "bounded new destination fixture command");
            section8b_test::backup_destination=section8b_test::backup_redesign=true;
            policy_argument=L"--section8b-followup-deadline-"+action;argv[1]=policy_argument.data();
        }
        const std::wstring two_value=argv[1],two_prefix=L"--backup-two-folder-";
        if(two_value.find(two_prefix)==0) {
            const auto action=two_value.substr(two_prefix.size());
            install::require(action==L"fresh-runtime" || action==L"runtime", "bounded two-folder Runtime command");
            section8b_test::backup_two_folder=section8b_test::backup_redesign=true;
            policy_argument=L"--section8b-followup-deadline-"+action;argv[1]=policy_argument.data();
        }
        const std::wstring backup_value=argv[1],backup_prefix=L"--backup-redesign-";
        if(backup_value.find(backup_prefix)==0) {
            const auto action=backup_value.substr(backup_prefix.size());
            install::require(action==L"fresh-runtime" || action==L"runtime", "bounded backup Runtime command");
            section8b_test::backup_redesign=true;
            policy_argument=L"--section8b-followup-deadline-"+action;argv[1]=policy_argument.data();
        }
        const std::wstring value=argv[1],client_prefix=L"--section8b-client-compat-",picker_prefix=L"--section8b-picker-diagnosis-";
        if(value.find(picker_prefix)==0){
            const auto action=value.substr(picker_prefix.size());
            if(action==L"lifecycle" || action==L"fresh-runtime" || action==L"runtime"){
                section8b_test::picker_diagnosis=section8b_test::policy_followup=true;
                policy_argument=L"--section8b-followup-deadline-"+action;argv[1]=policy_argument.data();
            }
        }
        const std::wstring empty_prefix=L"--section8b-empty-batch-";
        const bool empty=value.find(empty_prefix)==0,client=value.find(client_prefix)==0;
        const std::wstring prefix=empty?empty_prefix:(client?client_prefix:L"--section8b-policy-");
        if(value.find(prefix)==0){
            const auto action=value.substr(prefix.size());
            if(action==L"fault-lifecycle" || action==L"lifecycle" || action==L"fresh-runtime" || action==L"runtime"){
                section8b_test::empty_batch=empty;section8b_test::client_compat=client;section8b_test::policy_followup=true;policy_argument=L"--section8b-followup-deadline-"+action;argv[1]=policy_argument.data();
            }
        }
    }
    try {
        if(argc==2 && std::wstring(argv[1])==L"--section7-followup-create-conf") {
            create_followup_legacy_folder();return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--worker") {
            const std::wstring number(argv[3]);
            install::require(!number.empty() && number.size() <= 10 &&
                number.find_first_not_of(L"0123456789") == std::wstring::npos, "invalid worker PID");
            const auto pid = std::stoull(number);
            install::require(pid && pid <= std::numeric_limits<DWORD>::max() &&
                std::to_wstring(pid) == number, "invalid worker PID");
            return setup::worker(argv[2], static_cast<DWORD>(pid));
        }
        if(argc==2 && (std::wstring(argv[1])==L"--alternate-fresh" ||
            std::wstring(argv[1])==L"--alternate-update" || std::wstring(argv[1])==L"--alternate-uninstall" ||
            std::wstring(argv[1])==L"--alternate-start")) {
            alternate_drive(argv[1]); return 0;
        }
        if(argc==2 && std::wstring(argv[1])==L"--menu-lifecycle") {menu_setup_test::run();return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--menu-corrections-lifecycle") {menu_setup_test::run(true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--login-menu-lifecycle") {menu_setup_test::run(true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--login-menu-runtime") {login_menu_runtime();return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--welcome-placement-lifecycle") {menu_setup_test::run(true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--welcome-placement-runtime") {login_menu_runtime(true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-polish-lifecycle") {menu_setup_test::run(true,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-polish-runtime") {login_menu_runtime(false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-findings-lifecycle") {menu_setup_test::run(true,true,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-findings-runtime") {login_menu_runtime(false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-polish-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-polish-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-polish-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-help-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && (std::wstring(argv[1])==L"--section8b-lifecycle" || std::wstring(argv[1])==L"--section8b-fault-lifecycle")) {section8b_test::active=true;section8b_test::fault=std::wstring(argv[1])==L"--section8b-fault-lifecycle";menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-deadline-fault-lifecycle") {section8b_test::active=section8b_test::followup=section8b_test::deadline_followup=section8b_test::integration_fault=true;menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-deadline-lifecycle") {section8b_test::active=section8b_test::followup=section8b_test::deadline_followup=true;menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-deadline-fresh-runtime") {section8b_test::active=section8b_test::followup=section8b_test::deadline_followup=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-deadline-runtime") {section8b_test::active=section8b_test::followup=section8b_test::deadline_followup=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,false);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-ssh-fault-lifecycle") {section8b_test::active=section8b_test::followup=section8b_test::ssh_followup=section8b_test::integration_fault=true;menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-ssh-lifecycle") {section8b_test::active=section8b_test::followup=section8b_test::ssh_followup=true;menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-ssh-fresh-runtime") {section8b_test::active=section8b_test::followup=section8b_test::ssh_followup=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-ssh-runtime") {section8b_test::active=section8b_test::followup=section8b_test::ssh_followup=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,false);return 0;}
        if(argc==2 && (std::wstring(argv[1])==L"--section8b-followup-lifecycle" || std::wstring(argv[1])==L"--section8b-followup-probe-lifecycle")) {section8b_test::active=section8b_test::followup=true;section8b_test::fault=std::wstring(argv[1])==L"--section8b-followup-probe-lifecycle";menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-stock-lifecycle") {section8b_test::active=section8b_test::followup=section8b_test::stock_only=true;menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-fault-lifecycle") {section8b_test::active=section8b_test::followup=section8b_test::integration_fault=true;menu_setup_test::run(true,true,false,true,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-fresh-runtime") {section8b_test::active=section8b_test::followup=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-followup-runtime") {section8b_test::active=section8b_test::followup=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,false);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-fresh-runtime") {section8b_test::active=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8b-runtime") {section8b_test::active=true;login_menu_runtime(false,false,false,false,false,false,false,false,false,false,false,false,false,true,false);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-footer-lifecycle") {footer_format_test::active=true;menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-footer-runtime") {footer_format_test::active=true;login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-footer-fresh-runtime") {footer_format_test::active=true;login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-menu-followup-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-menu-followup-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-menu-followup-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-help-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-help-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-presentation-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-presentation-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-presentation-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-config-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-config-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-config-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-admin-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-admin-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-admin-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-login-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-login-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-login-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-corrections-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-corrections-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-corrections-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-offline") {section8a_offline();return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section8a-fresh-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-corrections-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-corrections-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-polish-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-polish-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-followup-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-followup-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-text-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,false,true,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section7-text-runtime") {login_menu_runtime(false,true,true,true,false,true,false,false,true,false,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section6-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section6-runtime") {login_menu_runtime(false,true,true,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section6-polish-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section6-polish-runtime") {login_menu_runtime(false,true,true,true,false,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--asleep-color-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--asleep-color-runtime") {login_menu_runtime(false,true,true,true,false,true,false,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-newsletter-lifecycle") {menu_setup_test::run(true,true,false,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-newsletter-runtime") {login_menu_runtime(false,true,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-october1-lifecycle") {menu_setup_test::run(true,true,false,true,true,true);return 0;}
        if(argc==2 && std::wstring(argv[1])==L"--section5-october1-runtime") {login_menu_runtime(false,true,true,true);return 0;}
        install::require(argc == 2 && std::wstring(argv[1]) == L"--fresh",
            "only fixed fresh qualification fixture is supported");
        install::require(!install::elevated(), "coordinator must run unelevated");
        const install::Path root(L"C:\\xtest-sfx-win11-clean-20260926");
        install::require(!std::filesystem::exists(root), "fixture root must be absent; never adopt or overwrite");
        setup::verify_payload();
        const auto plan = setup::inspect_plan(root, false);
        install::require(plan.operation == setup::Operation::fresh, "fresh plan required");
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        install::require(SUCCEEDED(com), "COM initialization failed");
        const bool done = setup::execute(plan, {false, false}, nullptr,
            [](const std::wstring& message) { std::wcout << message << std::endl; }, [] { return true; });
        CoUninitialize();
        install::require(done, "Windows approval cancelled; fixture not installed");
        const auto owner = install::operator_sid();
        install::verify_runtime_installation(root, owner);
        const auto receipt = install::read_receipt(root, owner);
        std::wcout << L"PASS genuine Setup engine fixture: " << root.native() << L" id=" << receipt.id
            << L" generation=" << receipt.generation << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL qualification Setup: " << error.what() << std::endl;
        return 1;
    }
}
