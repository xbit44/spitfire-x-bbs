#include "setup_engine.hpp"
#include "planning_task.hpp"
#include "resource.h"
#include "../product_version.h"
#include "sfx/platform/win32/window_icon.hpp"
#include "sfx/platform/win32/clamav_installation.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <algorithm>
#include <charconv>
#include <limits>
#include <string>

namespace {
namespace setup = sfx::apps::setup;
namespace install = sfx::platform::win32::installation;
struct Ui {
    explicit Ui(HINSTANCE instance)
        : large_icon(instance, IDI_SETUP, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)),
          small_icon(instance, IDI_SETUP, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON)) {}
    sfx::platform::win32::WindowIcon large_icon;
    sfx::platform::win32::WindowIcon small_icon;
    bool busy{false};
    bool cancelled{false};
    bool closed{false},close_after_checks{false},checking{false};
    bool requested_start_menu{false},requested_desktop{false};
    bool requested_full_removal{false};
    bool execute_started{false};
    std::wstring removal_id;
    std::wstring idle_status;
    ULONGLONG checking_started{};
    std::uint64_t review_requested_us{};
    setup::PlanningTask planning;
    std::vector<install::RegisteredInstallation> installations;
    std::filesystem::path first_run_root;
};
std::wstring text(HWND window, int control) {
    std::array<wchar_t, 1024> result{};
    GetDlgItemTextW(window, control, result.data(), static_cast<int>(result.size())); return result.data();
}
void error(HWND window, const char* code, const std::wstring& parent = {}) {
    const std::string ascii(code);
    if (ascii == "installation_recorded_target_unavailable_restore_drive_or_choose_existing_installation" ||
        ascii == "setup_uninstall_provenance_mismatch_preserve_data" ||
        ascii == "setup_uninstall_missing_original_volume_or_folder_changed" ||
        ascii == "setup_uninstall_selected_record_changed") {
        const auto message = L"Setup could not verify this recorded installation for the requested action.\n\n"
            L"Check that the original drive is connected and that the complete board is in the recorded folder. "
            L"If you intentionally moved or restored the complete board, open its Control Panel and review Prepare SFX on this computer. "
            L"For a separate test board, choose a new folder that has never been registered.\n\nDiagnostic: " +
            std::wstring(ascii.begin(),ascii.end());
        MessageBoxW(window, message.c_str(), L"Spitfire-X BBS Setup", MB_OK | MB_ICONWARNING);
        return;
    }
    if (ascii.find("installation_ancestor_") != std::string::npos ||
        ascii.find("installation_volume_root_unverified") != std::string::npos) {
        const auto message = L"Setup cannot safely use this installation location.\n\n" +
            (parent.empty() ? std::wstring{} : L"Parent location: " + parent + L"\n\n") +
            L"The parent folder's ownership or permissions could allow the installation to be changed, "
            L"or Windows could not verify its protection. Choose another local fixed NTFS location.\n\n"
            L"Setup has not changed the drive's permissions. Do not change permissions for the whole drive "
            L"to force installation. Keep any existing SFX data and backups.\n\nDiagnostic: " +
            std::wstring(ascii.begin(),ascii.end());
        MessageBoxW(window,message.c_str(),L"Spitfire-X BBS Setup",MB_OK | MB_ICONERROR);
        return;
    }
    const auto message = L"Setup did not complete.\n\n" + std::wstring(ascii.begin(), ascii.end()) +
        L"\n\nDo not delete any preserved source or recovery backup. Stop BBS explicitly, then close the Control Panel and SFXConfig before maintenance. "
        L"Setup never trusts an incomplete installation. If a retry cannot safely preserve and rebuild it, "
        L"keep the diagnostic and backups for recovery assistance.";
    MessageBoxW(window, message.c_str(), L"Spitfire-X BBS Setup", MB_OK | MB_ICONERROR);
}
constexpr UINT_PTR planning_timer=17;
void controls(HWND window,bool enabled){
    const auto* ui=reinterpret_cast<const Ui*>(GetWindowLongPtrW(window,DWLP_USER));
    const bool fixed=ui&&!ui->removal_id.empty();
    const bool remove=IsDlgButtonChecked(window,IDC_UNINSTALL)==BST_CHECKED;
    EnableWindow(GetDlgItem(window,IDOK),enabled);
    for(int id:{IDC_ROOT,IDC_BROWSE,IDC_UNINSTALL})EnableWindow(GetDlgItem(window,id),enabled&&!fixed);
    for(int id:{IDC_START_MENU,IDC_DESKTOP})EnableWindow(GetDlgItem(window,id),enabled&&!remove);
    EnableWindow(GetDlgItem(window,IDC_REMOVE_BOARD),enabled&&remove);
}
void show_progress(HWND window,const std::wstring& value){
    SetDlgItemTextW(window,IDC_PROGRESS,value.c_str());
    RedrawWindow(GetDlgItem(window,IDC_PROGRESS),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
}
struct RestoreIdle {
    HWND window;Ui* ui;
    ~RestoreIdle(){if(!ui->closed){ui->busy=false;ui->checking=false;if(ui->close_after_checks&&!ui->execute_started){ui->closed=true;EndDialog(window,0);return;}ui->cancelled=false;ui->close_after_checks=false;controls(window,true);if(!ui->execute_started)show_progress(window,ui->idle_status);}}
};
void launch_first_run(const std::filesystem::path& root) {
    const auto program = root / L"Bin" / install::programs[install::control_panel_program_index];
    try {
        const auto receipt = install::read_receipt(root, install::operator_sid());
        install::verify_structure(receipt);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        install::require(CreateProcessW(program.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
            nullptr, program.parent_path().c_str(), &startup, &process), "setup_control_panel_launch_failed");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } catch (const std::exception& failure) {
        const std::string diagnostic = failure.what();
        const auto message = L"Installation finished. The Control Panel could not open automatically.\n\n"
            L"Open this program to continue first-run setup:\n" + program.native() +
            L"\n\nDiagnostic: " + std::wstring(diagnostic.begin(), diagnostic.end());
        MessageBoxW(nullptr, message.c_str(), L"Spitfire-X BBS Setup", MB_OK | MB_ICONWARNING);
    }
}
INT_PTR CALLBACK dialog(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        SetWindowTextW(window, SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE L" Setup");
        ui = reinterpret_cast<Ui*>(lparam); SetWindowLongPtrW(window, DWLP_USER, lparam);
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ui->large_icon.get()));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ui->small_icon.get()));
        SendDlgItemMessageW(window, IDC_ROOT, CB_LIMITTEXT, 160, 0);
        try {
            ui->installations=ui->removal_id.empty()?install::registered_installations():
                install::registered_installations({ui->removal_id});
            const auto& installations=ui->installations;
            const auto suggested=install::suggested_installation_root(installations);
            SetDlgItemTextW(window,IDC_ROOT,suggested.c_str());
            for(const auto& entry:installations)
                SendDlgItemMessageW(window,IDC_ROOT,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(entry.receipt.root.c_str()));
            if(installations.empty()&&ui->removal_id.empty()) SetDlgItemTextW(window,IDC_ROOT,L"C:\\SFX");
            else if(!suggested.empty()) {
                const auto selected=std::find_if(installations.begin(),installations.end(),[&](const auto& entry){
                    return !_wcsicmp(entry.receipt.root.c_str(),suggested.c_str());});
                SendDlgItemMessageW(window,IDC_ROOT,CB_SETCURSEL,selected-installations.begin(),0);
                SetDlgItemTextW(window,IDC_PROGRESS,selected->available
                    ? L"Recorded SFX installation selected. Review verifies it again before maintenance."
                    : L"Recorded installation is unavailable or changed. Check its drive/path; Setup will not select a new C:\\SFX installation.");
            } else {
                SetDlgItemTextW(window,IDC_ROOT,L"");
                SetDlgItemTextW(window,IDC_PROGRESS,L"Multiple SFX installations are recorded. Choose the installation to maintain from the folder list; no target has been selected.");
            }
        } catch(const std::exception&) {
            SetDlgItemTextW(window,IDC_ROOT,L"");
            SetDlgItemTextW(window,IDC_PROGRESS,L"Setup could not safely read installation records. Select the existing installation explicitly. It has not defaulted to a new C:\\SFX installation.");
        }
        CheckDlgButton(window, IDC_START_MENU, BST_CHECKED); CheckDlgButton(window, IDC_DESKTOP, BST_CHECKED);
        if(!ui->removal_id.empty()) {
            SetWindowTextW(window,L"Remove Spitfire-X BBS");
            CheckDlgButton(window,IDC_UNINSTALL,BST_CHECKED);
            SetDlgItemTextW(window,IDC_PROGRESS,L"Review the selected installation. By default, Setup keeps your BBS data and backups.");
        }
        controls(window,true);
        return TRUE;
    }
    if (!ui) return FALSE;
    if(message==WM_DESTROY){KillTimer(window,planning_timer);ui->closed=true;ui->cancelled=true;ui->planning.cancel();return TRUE;}
    if(ui->closed)return FALSE;
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) {
        if(ui->checking){
            ui->close_after_checks=true;ui->planning.cancel();
            show_progress(window,L"Stopping installation checks...\nSetup will close when the current read finishes.");
        }else if(ui->busy&&!ui->execute_started){ui->close_after_checks=true;ui->cancelled=true;show_progress(window,L"Closing review... Dismiss the current question if one is open.");}
        else if (ui->busy) { ui->cancelled = true; SetDlgItemTextW(window, IDC_PROGRESS, L"Cancelling safely; preserved data will not be removed..."); }
        else{ui->closed=true;EndDialog(window, 0);}
        return TRUE;
    }
    const bool checked=message==WM_TIMER&&wparam==planning_timer&&ui->planning.active();
    if(checked){
        if(!ui->planning.ready()){
            if(!ui->close_after_checks)show_progress(window,setup::planning_status(ui->planning.phase(),GetTickCount64()-ui->checking_started));
            return TRUE;
        }
        KillTimer(window,planning_timer);
        ui->checking=false;
        if(ui->close_after_checks){
            try{(void)ui->planning.take();}catch(...){}
            ui->closed=true;ui->busy=false;EndDialog(window,0);return TRUE;
        }
    }else{
    if (message != WM_COMMAND) return FALSE;
    if(ui->busy)return TRUE; // Consume duplicate Review; never run another plan.
    if(LOWORD(wparam)==IDC_UNINSTALL){
        if(IsDlgButtonChecked(window,IDC_UNINSTALL)!=BST_CHECKED)CheckDlgButton(window,IDC_REMOVE_BOARD,BST_UNCHECKED);
        controls(window,true);
        return TRUE;
    }
    if (LOWORD(wparam) == IDC_BROWSE) {
        BROWSEINFOW browse{}; browse.hwndOwner = window;
        browse.lpszTitle = L"Select an existing SFX folder, or edit the path to create a new folder on a suitable fixed volume.";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        auto pidl = SHBrowseForFolderW(&browse);
        if (pidl) {
            std::array<wchar_t, MAX_PATH> path{};
            if (SHGetPathFromIDListW(pidl, path.data())) SetDlgItemTextW(window, IDC_ROOT, path.data());
            CoTaskMemFree(pidl);
        }
        return TRUE;
    }
    if (LOWORD(wparam) != IDOK) return FALSE;
    try{
        ui->review_requested_us=setup::PlanningTask::now_us();
        const auto selected_root=text(window,IDC_ROOT);
        const bool uninstall=IsDlgButtonChecked(window,IDC_UNINSTALL)==BST_CHECKED;
        ui->requested_full_removal=uninstall&&IsDlgButtonChecked(window,IDC_REMOVE_BOARD)==BST_CHECKED;
        ui->requested_start_menu=IsDlgButtonChecked(window,IDC_START_MENU)==BST_CHECKED;
        ui->requested_desktop=IsDlgButtonChecked(window,IDC_DESKTOP)==BST_CHECKED;
        ui->busy=true;ui->checking=true;ui->cancelled=false;ui->close_after_checks=false;ui->checking_started=GetTickCount64();
        ui->execute_started=false;ui->idle_status=L"Review cancelled. You may review again or close Setup.";
        controls(window,false);
        show_progress(window,setup::planning_status(setup::PlanningPhase::embedded_payload,0));
        if(ui->closed)return TRUE;
        if(ui->close_after_checks){ui->closed=true;ui->busy=false;ui->checking=false;EndDialog(window,0);return TRUE;}
        install::require(SetTimer(window,planning_timer,100,nullptr)!=0,"setup_review_timer_failed");
        auto removal_id=ui->removal_id;
        if(uninstall&&removal_id.empty()){
            const auto match=std::find_if(ui->installations.begin(),ui->installations.end(),[&](const auto& entry){
                return !entry.available && !_wcsicmp(entry.receipt.root.c_str(),selected_root.c_str());
            });
            if(match!=ui->installations.end())removal_id=match->receipt.id;
        }
        if(removal_id.empty())ui->planning.start(selected_root,uninstall);
        else ui->planning.start(selected_root,true,[removal_id](const auto&,bool,const auto& callbacks){
            return setup::inspect_registered_uninstall(removal_id,callbacks);
        });
    }catch(const std::exception& failure){KillTimer(window,planning_timer);ui->busy=false;ui->checking=false;controls(window,true);show_progress(window,L"Installation checks could not start. Follow the diagnostic before retrying.");error(window,failure.what());}
    return TRUE;
    }
    RestoreIdle restore{window,ui};
    try {
        auto plan = ui->planning.take(); // Joins before any choices or installation.
        show_progress(window,L"Installation checks complete. Review your choices before continuing.");
        if(ui->close_after_checks||ui->closed)return TRUE;
        if (plan.operation == setup::Operation::maintenance) {
            const auto backup_choice = MessageBoxW(window,
                L"Create and verify a mutable-BBS-data backup before updating?\n\n"
                L"Yes: save in Backup\\Upgrades and retain the newest three verified managed sets.\n"
                L"No: update without this optional data backup. You are responsible for having another usable backup.\n\n"
                L"Temporary protected program recovery remains mandatory. This is not a full-system snapshot.",
                L"Spitfire-X BBS — Upgrade Backup", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
            if (backup_choice == IDCANCEL) return TRUE;
            if (backup_choice == IDNO) plan.backup.clear();
        }
        if(ui->close_after_checks||ui->closed)return TRUE;
        if(plan.operation==setup::Operation::fresh && install::recorded_installation_root(ui->installations,plan.root))
            throw std::runtime_error("installation_recorded_target_unavailable_restore_drive_or_choose_existing_installation");
        if(plan.operation==setup::Operation::uninstall && ui->requested_full_removal)
            plan.operation=setup::Operation::uninstall_full;
        setup::Options options;
        options.install_uninstall_entry=true;
        if(sfx::platform::win32::clamav_available() && !setup::removing(plan.operation)){
            const auto existing=sfx::platform::win32::clamav_installed(plan.root);
            bool upgrade{};
            if(existing&&plan.previous){
                upgrade=sfx::platform::win32::clamav_upgrade_required(plan.root,plan.previous->owner,plan.previous->id);
                if(!upgrade)try{
                    const auto predecessor=sfx::platform::win32::confirmed_preparation_predecessor(*plan.previous);
                    if(predecessor)upgrade=sfx::platform::win32::clamav_upgrade_required(plan.root,
                        plan.previous->owner,plan.previous->id,*predecessor);
                }catch(const std::exception&){} // The stopped worker reports unverified component history.
            }
            if(!existing||upgrade){
            const auto choice=MessageBoxW(window,
                upgrade?
                L"Upgrade Portable ClamAV to 1.4.7 now?\n\nYes: download and verify the supported engine (about 163 MiB). Existing definitions and Scan Uploads settings are kept.\n\nNo: keep the existing files and upgrade later in SFXConfig > Security > ClamAV. This SFX version requires 1.4.7. With Scan Uploads On, uploads are held privately until a required scan can finish.\n\nCancel: return to Setup.":
                L"Install Portable ClamAV now?\n\nYes: download and verify the supported engine (about 163 MiB), then try to download the initial definitions. Check the result for definition readiness. Runtime checks for updates daily.\n\nNo: skip for now. You can install it later in SFXConfig > Security > ClamAV.\n\nScan Uploads is a separate setting. Existing choices are kept. When scanning is On and a required scan cannot finish, the upload is held privately for review.\n\nCancel: return to Setup.",
                L"Optional ClamAV",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON2);
            if(choice==IDCANCEL)return TRUE;
            if(choice==IDYES)options.automatic_clamav=true;
            }
        }
        if(ui->close_after_checks||ui->closed)return TRUE;
        const auto mode_notice=L"\n\nFresh installations use Desktop Mode with the registered service Disabled. Updates preserve the selected mode. Change modes later in SFXConfig > System > Runtime Behavior with the BBS stopped.";
        const auto confirm = setup::summary(plan) + (setup::removing(plan.operation)?L"":mode_notice) +
            (options.automatic_clamav?L"\n\nPortable ClamAV automatic download/install and initial definitions selected.":L"") +
            L"\n\nContinue? Setup will request normal Windows administrator approval when needed.";
        if (MessageBoxW(window, confirm.c_str(), L"Review SFX installation", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
            return TRUE;
        if(ui->close_after_checks||ui->closed)return TRUE;
        options.start_menu = ui->requested_start_menu;
        options.desktop = ui->requested_desktop;
        ui->busy = true; ui->cancelled = false;
        const auto progress = [window,ui](const std::wstring& value) {
            if(!ui->closed)show_progress(window,value);
        };
        const auto pump = [window, ui] {
            MSG queued{};
            while (PeekMessageW(&queued, nullptr, 0, 0, PM_REMOVE)) {
                if (queued.message == WM_QUIT) ui->cancelled = true;
                else if (!IsDialogMessageW(window, &queued)) { TranslateMessage(&queued); DispatchMessageW(&queued); }
            }
            return !ui->cancelled;
        };
        ui->execute_started=true;
        const bool complete = setup::execute(plan, options, window, progress, pump);
        if (!complete) progress(L"Windows approval was cancelled. Your installation and source were not changed. You may retry.");
        else {
            if (plan.operation == setup::Operation::fresh) ui->first_run_root = plan.root;
            else MessageBoxW(window, text(window, IDC_PROGRESS).c_str(),
                L"Spitfire-X BBS Setup", MB_OK | MB_ICONINFORMATION);
            // The worker and maintenance locks are gone. Close the Setup UI
            // before WinMain hands the verified fresh board to Control Panel.
            ui->busy = false;
            ui->closed = true;
            EndDialog(window, 0);
            return TRUE;
        }
    } catch (const setup::PlanningCancelled&) {
        ui->idle_status=L"Installation checks cancelled. You may review again or close Setup.";
    } catch (const install::AncestorSecurityError& failure) {
        ui->idle_status=L"Installation checks did not finish. Follow the diagnostic before retrying.";
        error(window, failure.what(), failure.path().native());
    } catch (const std::exception& failure) { ui->idle_status=L"Installation checks did not finish. Follow the diagnostic before retrying.";error(window, failure.what()); }
    catch (...) { ui->idle_status=L"Review did not finish. Follow the diagnostic before retrying.";error(window,"setup_planning_or_installation_failed"); }
    return TRUE;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0; auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 2;
    std::vector<std::wstring> arguments(args, args + count); LocalFree(args);
    if (count == 4 && arguments[1] == L"--worker") {
        const auto& number = arguments[3];
        if (number.empty() || number.size() > 10 || number.find_first_not_of(L"0123456789") != std::wstring::npos) return 3;
        const auto pid = wcstoull(number.c_str(), nullptr, 10);
        if (!pid || pid > std::numeric_limits<DWORD>::max() || std::to_wstring(pid) != number) return 3;
        return setup::worker(arguments[2], static_cast<DWORD>(pid));
    }
    // This read-only package-tool command must never open UI or await a click,
    // including when deliberately checking damaged media. It cannot install.
    if (count == 2 && arguments[1] == L"--verify-payload") {
        try { setup::verify_payload(); return 0; } catch (const std::exception&) { return 5; }
    }
    try {
        const bool uninstall=count==3&&arguments[1]==L"--uninstall"&&arguments[2].size()==32&&
            arguments[2].find_first_not_of(L"0123456789ABCDEF")==std::wstring::npos;
        install::require(count == 1 || uninstall, "setup_arguments_invalid");
        install::require(!install::elevated(), "setup_start_normally_not_as_administrator");
        setup::verify_payload();
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        install::require(SUCCEEDED(com), "setup_shell_initialization_failed");
        Ui ui(instance);
        if(uninstall)ui.removal_id=arguments[2];
        const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETUP), nullptr, dialog,
            reinterpret_cast<LPARAM>(&ui)); CoUninitialize();
        if (result != -1 && !ui.first_run_root.empty()) launch_first_run(ui.first_run_root);
        return result == -1 ? 4 : 0;
    } catch (const std::exception& failure) { error(nullptr, failure.what()); return 5; }
}
