// Real Config controls, synthetic display-only state; no installation or save.
#define SFX_ALPHA_TEST_AUTOMATION 1
#define wWinMain unused_config_entry
#include "../../src/apps/config_ui/main.cpp"
#undef wWinMain
#include <iostream>
namespace sfx::platform::win32 {ClamAvHost clamav_host() noexcept{return ClamAvHost::windows7_no_av;}}
namespace {
LRESULT CALLBACK noav_proc(HWND w,UINT m,WPARAM a,LPARAM b){if(m==WM_CREATE)return 0;return window_proc(w,m,a,b);}
BOOL CALLBACK noav_caption(HWND child,LPARAM raw){auto* bad=reinterpret_cast<bool*>(raw);wchar_t caption[2048]{};GetWindowTextW(child,caption,2048);if(std::wstring(caption).find(L"ClamAV")!=std::wstring::npos||std::wstring(caption).find(L"Scan caller uploads")!=std::wstring::npos)*bad=true;return TRUE;}
}
int main(){State s;s.instance=GetModuleHandleW(nullptr);try{
    unsigned n{};const auto check=[&](bool x,const char* why){++n;if(!x)throw std::runtime_error(why);std::cout<<"PASS "<<why<<'\n';};
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"common controls");
    WNDCLASSW cls{};cls.hInstance=s.instance;cls.lpfnWndProc=noav_proc;cls.lpszClassName=kWindowClass;check(RegisterClassW(&cls)!=0,"isolated production control class");
    const auto w=CreateWindowW(kWindowClass,L"Win7 policy native Config test",WS_OVERLAPPEDWINDOW,0,0,980,750,nullptr,nullptr,s.instance,&s);
    check(w&&create_ui(s),"real Config UI succeeds without AV page");
    check(!s.pages[25]&&!s.file_editor.scan&&!s.clam_detail&&!s.clam_install&&!s.clam_update,"no AV controls or page created");
    check(TabCtrl_GetItemCount(s.security_tabs)==3,"only three non-AV Security tabs");
    bool bad{};EnumChildWindows(w,noav_caption,reinterpret_cast<LPARAM>(&bad));check(!bad,"no ClamAV caption or scanning option in any child control");
    show_page(s,25);check(s.current_page==11,"stale AV route falls back to BBS Security");
    start_clam_job(s,true);start_clam_job(s,false);install_clam(s);check(!s.clam_job&&!s.clam_maintenance,"direct hidden commands start no worker or installer");
    DestroyWindow(w);std::cout<<"PASS Win7 Config "<<n<<" native control checks; simulated OS policy, not physical Win7/pixel acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';if(s.window)DestroyWindow(s.window);return 1;}}
