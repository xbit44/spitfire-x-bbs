// Exercise the actual Setup WM_INITDIALOG against its native resource.
// EndDialog during initialization, before any window is shown or action runs.
#include "setup_engine.cpp"
#include "../../src/apps/setup/main.cpp"
#include <iostream>
namespace {
std::wstring observed;bool visible{};
INT_PTR CALLBACK inspect_caption(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    if(message!=WM_INITDIALOG)return FALSE;
    (void)dialog(window,message,wparam,lparam);wchar_t title[256]{};GetWindowTextW(window,title,256);observed=title;visible=IsWindowVisible(window)!=FALSE;EndDialog(window,123);return FALSE;
}
}
int wmain(){
    Ui ui(GetModuleHandleW(nullptr));const auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_SETUP),nullptr,inspect_caption,reinterpret_cast<LPARAM>(&ui));
    const bool okay=result==123&&!visible&&observed==SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE L" Setup";
    std::wcout<<(okay?L"PASS ":L"FAIL ")<<L"actual hidden native Setup caption: "<<observed<<L"; no install action or visible dialog\n";return okay?0:1;
}
