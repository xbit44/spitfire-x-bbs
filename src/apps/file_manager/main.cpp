#include "window.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <commctrl.h>
#include <objbase.h>
#include <cwctype>
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR arguments,int)try{
    namespace ins=sfx::platform::win32::installation;
    // The matched installed module defines its root. No arbitrary-root CLI,
    // elevated recovery mode, SQL switch, automation mode or test password.
    for(const auto* c=arguments;c&&*c;++c)ins::require(std::iswspace(*c)!=0,"file_manager_unexpected_arguments");
    ins::require(!ins::elevated(),"file_manager_requires_ordinary_owner");const auto owner=ins::operator_sid();const auto module=ins::module_path();const auto root=ins::canonical_root(module.parent_path().parent_path());const auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);ins::require(receipt.program_count>ins::file_manager_program_index,"file_manager_requires_matched_setup");ins::verify_installed_program_process(receipt,GetCurrentProcessId(),ins::file_manager_program_index);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_TREEVIEW_CLASSES|ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};ins::require(InitCommonControlsEx(&controls)!=FALSE,"file_manager_common_controls_unavailable");const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);ins::require(SUCCEEDED(initialized),"file_manager_shell_dialogs_unavailable");struct ComEnd{~ComEnd(){CoUninitialize();}} end;
    return sfx::file_manager::run_file_manager_ui(instance,root,owner);
}catch(...){MessageBoxW(nullptr,L"File Manager could not open this matched installation. Run it normally from the installation's Bin folder after updating with SFXSetup. Do not run it as Administrator.",L"Spitfire-X File Manager",MB_OK|MB_ICONERROR);return 1;}
