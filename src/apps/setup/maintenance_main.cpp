#include "clamav_maintenance.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include <shellapi.h>
#include <cwchar>

namespace {
namespace ins=sfx::platform::win32::installation;
struct Window {std::filesystem::path root;bool busy{},cancel{};};
INT_PTR CALLBACK dialog(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    auto* state=reinterpret_cast<Window*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG){state=reinterpret_cast<Window*>(lparam);SetWindowLongPtrW(window,DWLP_USER,lparam);SetDlgItemTextW(window,101,state->root.c_str());return TRUE;}
    if(!state)return FALSE;
    if(message==WM_CLOSE){if(state->busy){state->cancel=true;SetDlgItemTextW(window,102,L"Cancellation requested. Waiting for the current safe boundary...");}else EndDialog(window,0);return TRUE;}
    if(message!=WM_COMMAND)return FALSE;
    if(LOWORD(wparam)==IDCANCEL){SendMessageW(window,WM_CLOSE,0,0);return TRUE;}
    if(LOWORD(wparam)!=IDOK||state->busy)return FALSE;
    state->busy=true;state->cancel=false;EnableWindow(GetDlgItem(window,IDOK),FALSE);SetDlgItemTextW(window,IDCANCEL,L"Cancel");
    const auto progress=[&](const std::wstring& text){SetDlgItemTextW(window,102,text.c_str());};
    const auto pump=[&]{MSG item{};while(PeekMessageW(&item,nullptr,0,0,PM_REMOVE)){if(item.message==WM_QUIT){state->cancel=true;continue;}if(!IsDialogMessageW(window,&item)){TranslateMessage(&item);DispatchMessageW(&item);}}return !state->cancel;};
    try{if(!sfx::apps::setup::install_portable_clamav(state->root,window,progress,pump))progress(L"Windows approval cancelled. No component was installed.");}
    catch(const std::exception& e){const std::string raw=e.what();std::wstring text(raw.begin(),raw.end());progress(L"Maintenance did not finish: "+text+L"\r\nA published component is preserved. Refresh ClamAV status before retrying.");}
    state->busy=false;EnableWindow(GetDlgItem(window,IDOK),TRUE);SetDlgItemTextW(window,IDCANCEL,L"Close");return TRUE;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    if(!sfx::platform::win32::clamav_available())return 0;
    try{int count{};auto** args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 2;struct Free{LPWSTR* p;~Free(){LocalFree(p);}} free{args};
        if(count==4&&std::wstring(args[1])==L"--worker"){wchar_t* end{};const auto pid=std::wcstoul(args[3],&end,10);if(!pid||!end||*end)return 2;return sfx::apps::setup::clamav_maintenance_worker(args[2],pid);}
        ins::require(count==1&&!ins::elevated(),"Start SFX Maintenance normally, without arguments or Run as administrator.");
        Window state;state.root=ins::canonical_root(ins::module_path().parent_path().parent_path());const auto receipt=ins::read_receipt(state.root,ins::operator_sid());ins::verify_structure(receipt);ins::verify_installed_program_process(receipt,GetCurrentProcessId(),ins::maintenance_program_index);
        return static_cast<int>(DialogBoxParamW(instance,MAKEINTRESOURCEW(301),nullptr,dialog,reinterpret_cast<LPARAM>(&state)));
    }catch(const std::exception& e){const std::string raw=e.what();const std::wstring text(raw.begin(),raw.end());MessageBoxW(nullptr,text.c_str(),L"Spitfire-X Portable ClamAV Maintenance",MB_OK|MB_ICONERROR);return 2;}
}
