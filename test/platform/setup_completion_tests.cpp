// I11: owner-requested completion UX. Exercise the actual dialog handler with
// fake installer and window boundaries: no GUI, UAC, files, registry or callers.
// Accepted install/ACL evidence cannot prove this new success-exit branch.
#include "../../src/apps/setup/setup_engine.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <cwchar>

namespace completion_test {
enum class Outcome { success, cancelled, failure, planning_error, planning_cancel, blocked };
Outcome outcome{};
void* context = nullptr;
unsigned acknowledgements = 0, errors = 0, closes = 0, enabled = 0, executed = 0;
bool close_after_acknowledgement = false;
std::atomic<unsigned> inspected{0},wrong_thread{0};
std::atomic<bool> entered{false},retired{false};
DWORD ui_thread{};bool fail_timer=false;
bool maintenance=false,review_no=false;
int clamav_choice=IDNO;
std::function<void()> during_status;
std::vector<std::wstring> status;
std::wstring last_notice;
LONG_PTR userdata(HWND, int) { return reinterpret_cast<LONG_PTR>(context); }
void ui_only(){if(GetCurrentThreadId()!=ui_thread)++wrong_thread;}
HWND control(HWND,int id){ui_only();return reinterpret_cast<HWND>(static_cast<INT_PTR>(id));}
int read_text(HWND,int,LPWSTR value,int size){ui_only();wcsncpy_s(value,size,L"synthetic selected root",_TRUNCATE);return static_cast<int>(wcslen(value));}
UINT checked(HWND,int){ui_only();return BST_UNCHECKED;}
UINT_PTR timer(HWND,UINT_PTR id,UINT,TIMERPROC){ui_only();return fail_timer?0:id;}
BOOL kill(HWND,UINT_PTR){ui_only();return TRUE;}
BOOL redraw(HWND,const RECT*,HRGN,UINT){ui_only();return TRUE;}
int notice(HWND, LPCWSTR message, LPCWSTR title, UINT flags) {
    ui_only();
    last_notice = message;
    if(std::wstring(title)==L"Optional ClamAV")return clamav_choice;
    if(std::wstring(title).find(L"Upgrade Backup")!=std::wstring::npos)return IDCANCEL;
    if (std::wstring(title) == L"Review SFX installation") return review_no?IDNO:IDYES;
    if (flags & MB_ICONERROR) ++errors;
    else ++acknowledgements;
    return IDOK;
}
BOOL close(HWND, INT_PTR) { ui_only();++closes; close_after_acknowledgement = acknowledgements == 1; return TRUE; }
BOOL enable(HWND, BOOL value) { ui_only();if (value) ++enabled; return TRUE; }
BOOL progress(HWND, int, LPCWSTR value) { ui_only();status.emplace_back(value);if(during_status){auto callback=std::move(during_status);during_status={};callback();}return TRUE; }
void reset(Outcome value) {
    outcome = value; acknowledgements = errors = closes = enabled = executed = 0;
    close_after_acknowledgement = false; status.clear();
    inspected=0;wrong_thread=0;entered=false;retired=false;ui_thread=GetCurrentThreadId();fail_timer=false;maintenance=false;review_no=false;clamav_choice=IDNO;during_status={};
}
}

// Windows headers are already parsed above. These redirects intercept only
// the production handler's presentation boundaries in this test translation unit.
#undef GetWindowLongPtrW
#define GetWindowLongPtrW completion_test::userdata
#define GetDlgItem completion_test::control
#define GetDlgItemTextW completion_test::read_text
#define IsDlgButtonChecked completion_test::checked
#define SetTimer completion_test::timer
#define KillTimer completion_test::kill
#define RedrawWindow completion_test::redraw
#define MessageBoxW completion_test::notice
#define EndDialog completion_test::close
#define EnableWindow completion_test::enable
#define SetDlgItemTextW completion_test::progress
#define wWinMain completion_unused_gui_entry
#include "../../src/apps/setup/main.cpp"
#undef wWinMain
#undef RedrawWindow
#undef KillTimer
#undef SetTimer
#undef IsDlgButtonChecked
#undef GetDlgItemTextW
#undef GetDlgItem
#undef SetDlgItemTextW
#undef EnableWindow
#undef EndDialog
#undef MessageBoxW
#undef GetWindowLongPtrW

namespace sfx::platform::win32 {
ClamAvHost clamav_host()noexcept{return ClamAvHost::enabled;}
bool clamav_installed(const std::filesystem::path&){return false;}
}
namespace sfx::apps::setup {
void verify_payload() { throw std::runtime_error("unexpected media call in dialog-only test"); }
Plan inspect_plan(const install::Path&, bool,const PlanningCallbacks& callbacks) {
    using namespace completion_test;++inspected;callbacks.status(PlanningPhase::installed_files);entered=true;
    if(outcome==Outcome::blocked){while(callbacks.keep_going())std::this_thread::yield();retired=true;throw PlanningCancelled();}
    if(outcome==Outcome::planning_error)throw std::runtime_error("synthetic planning error");
    if(outcome==Outcome::planning_cancel)throw PlanningCancelled();
    Plan result;result.operation=maintenance?Operation::maintenance:Operation::fresh;return result;
}
Plan inspect_plan(const install::Path& root, bool remove) { return inspect_plan(root,remove,{}); }
std::wstring summary(const Plan&) { return L"Completion-test review"; }
bool execute(const Plan&, const Options&, HWND, Progress progress, Pump) {
    using namespace completion_test;
    ui_only();
    ++executed;
    if (outcome == Outcome::failure) throw std::runtime_error("completion-fixture-failure");
    if (outcome == Outcome::cancelled) return false;
    progress(L"Installation complete. Start Spitfire-X BBS from your selected shortcut.");
    return true;
}
int worker(const std::wstring&, DWORD) noexcept { return 99; }
}

int wmain() {
    unsigned checks = 0;
    const auto require = [&](bool value, const char* label) {
        ++checks; if (!value) throw std::runtime_error(label);
    };
    try {
        using namespace completion_test;
        const auto complete=[&](Ui& ui){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!ui.planning.ready()){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("planning timeout");std::this_thread::yield();}require(dialog(nullptr,WM_TIMER,planning_timer,0)==TRUE,"ready timer handled");};
        for (const auto result : {Outcome::success, Outcome::cancelled, Outcome::failure}) {
            reset(result); Ui ui(nullptr); context = &ui;
            require(dialog(nullptr, WM_COMMAND, IDOK, 0) == TRUE, "handler returns handled");
            require(ui.busy&&ui.checking&&executed==0&&!status.empty()&&status.front().find(L"Checking installation...")==0,"feedback and busy state before installer");
            require(dialog(nullptr,WM_COMMAND,IDOK,0)==TRUE,"duplicate Review consumed");complete(ui);
            require(inspected==1&&wrong_thread==0,"one inspector; all controls and installer stay on UI thread");
            const auto trace=ui.planning.last_timing();
            require(ui.review_requested_us<=trace.requested_us&&trace.requested_us<=trace.started_us&&trace.started_us<=trace.finished_us&&trace.finished_us<=trace.consumed_us,"monotonic handler/queued/start/finish/result-consumption boundaries");
            if(result==Outcome::success)std::cout<<"HANDLER_TASK_TIMING {\"handler_to_queue_us\":"<<trace.requested_us-ui.review_requested_us<<",\"queue_to_start_us\":"<<trace.started_us-trace.requested_us<<",\"synthetic_checks_us\":"<<trace.finished_us-trace.started_us<<",\"finish_to_consumption_us\":"<<trace.consumed_us-trace.finished_us<<"}; mocked UI/checks, not shipping GUI timing\n";
            require(executed == 1 && !ui.busy, "one operation, no lingering busy state");
            if (result == Outcome::success) {
                require(acknowledgements == 1 && errors == 0, "success acknowledgement exactly once");
                require(closes == 1 && close_after_acknowledgement, "OK closes Setup after acknowledgement");
                require(enabled == 0, "no control operations after closing Setup");
            } else if (result == Outcome::cancelled) {
                require(closes == 0 && acknowledgements == 0 && errors == 0, "No keeps Setup open without success");
                require(enabled == 6 && !status.empty() && status.back().find(L"You may retry") != std::wstring::npos,
                    "cancellation restores retry controls and wording");
            } else {
                require(closes == 0 && acknowledgements == 0 && errors == 1, "failure stays open with diagnostic");
                require(enabled == 6, "failure restores retry controls");
            }
        }
        {reset(Outcome::planning_error);Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(errors==1&&executed==0&&!ui.busy&&!ui.planning.active(),"planning failure restores retry without installer");require(status.back().find(L"did not finish")!=std::wstring::npos,"planning error replaces stale checking status");outcome=Outcome::success;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(executed==1&&closes==1,"planning error retry completes once");}
        {reset(Outcome::success);maintenance=true;Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(executed==0&&!ui.busy&&enabled==6&&status.back().find(L"Review cancelled")!=std::wstring::npos,"backup Cancel restores honest idle status and controls");}
        {reset(Outcome::success);review_no=true;Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(executed==0&&!ui.busy&&status.back().find(L"Review cancelled")!=std::wstring::npos,"summary No clears completed checker status");}
        {reset(Outcome::success);clamav_choice=IDCANCEL;Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(executed==0&&!ui.busy&&enabled==6&&status.back().find(L"Review cancelled")!=std::wstring::npos,"Optional ClamAV Cancel restores honest idle controls/status");}
        {reset(Outcome::success);Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!ui.planning.ready()){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("result timeout");std::this_thread::yield();}during_status=[&]{dialog(nullptr,WM_CLOSE,0,0);};complete(ui);require(closes==1&&executed==0&&acknowledgements==0,"Close while consuming checked result opens no question or installer");}
        {reset(Outcome::planning_cancel);Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(errors==0&&executed==0&&!ui.busy&&enabled==6,"planning cancellation is not a provenance failure");}
        {reset(Outcome::blocked);Ui ui(nullptr);context=&ui;dialog(nullptr,WM_COMMAND,IDOK,0);const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!entered){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("arrival timeout");std::this_thread::yield();}dialog(nullptr,WM_TIMER,planning_timer,0);require(ui.checking&&executed==0,"timer remains responsive while checker blocked");dialog(nullptr,WM_CLOSE,0,0);require(closes==0&&ui.close_after_checks,"Close waits for checker");complete(ui);require(retired&&closes==1&&executed==0&&acknowledgements==0&&enabled==0,"Close cancels and joins before destruction");}
        {reset(Outcome::success);Ui ui(nullptr);context=&ui;during_status=[&]{dialog(nullptr,WM_CLOSE,0,0);};dialog(nullptr,WM_COMMAND,IDOK,0);require(closes==1&&inspected==0&&!ui.planning.active(),"Close during first paint starts no orphan task");}
        {reset(Outcome::success);Ui ui(nullptr);context=&ui;fail_timer=true;dialog(nullptr,WM_COMMAND,IDOK,0);require(errors==1&&!ui.busy&&!ui.checking&&!ui.planning.active(),"timer failure restores idle without task");fail_timer=false;dialog(nullptr,WM_COMMAND,IDOK,0);complete(ui);require(executed==1,"retry after timer failure");}
        context=nullptr;
        error(nullptr,"installation_ancestor_writable",L"D:\\Shared");
        require(last_notice.find(L"D:\\Shared") != std::wstring::npos &&
            last_notice.find(L"Choose another local fixed NTFS location") != std::wstring::npos &&
            last_notice.find(L"has not changed the drive's permissions") != std::wstring::npos,
            "unsafe location identifies parent and safe next step without recommending drive ACL changes");
        error(nullptr,"installation_volume_root_unverified",L"D:\\");
        require(last_notice.find(L"Windows could not verify") != std::wstring::npos,
            "uncertain volume verification has understandable refusal");
        std::cout << "SETUP COMPLETION PASS checks=" << checks << " failed=0 skipped=0; no GUI/UAC/install\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SETUP COMPLETION FAIL checks=" << checks << " diagnostic=" << error.what() << '\n';
        return 1;
    }
}
