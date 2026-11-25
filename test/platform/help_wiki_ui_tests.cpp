// Bounded native menu checks. No Runtime, browser, or installed fixture is started.
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
namespace wiki_review {
std::filesystem::path root;
bool throw_root{};
INT_PTR launch_result=33;
unsigned launches{}, messages{}, commands{}, checks{};
std::wstring file, verb, directory, message;
HWND owner{};
HMENU popup{};
bool mouse{};
void check(bool value,const char* label) {
    ++checks; if(!value) throw std::runtime_error(label);
    std::cout << "PASS " << label << std::endl;
}
HINSTANCE WINAPI launch(HWND,LPCWSTR operation,LPCWSTR target,LPCWSTR arguments,LPCWSTR cwd,INT) {
    check(arguments==nullptr,"no command-line shell interpolation");
    ++launches; verb=operation; file=target; directory=cwd;
    return reinterpret_cast<HINSTANCE>(launch_result);
}
int WINAPI notice(HWND,LPCWSTR text,LPCWSTR,UINT) {++messages;message=text;return IDOK;}
}
namespace sfx::platform::win32 {
std::filesystem::path wiki_review_root() {
    if(wiki_review::throw_root)throw std::runtime_error("synthetic root failure");
    return wiki_review::root;
}
}
#define alpha_installation_root wiki_review_root
#define ShellExecuteW wiki_review::launch
#define MessageBoxW wiki_review::notice
#define wWinMain unused_control_panel_entry
#include "../../src/apps/control_panel/main.cpp"
#undef wWinMain
#undef MessageBoxW
#undef ShellExecuteW
#undef alpha_installation_root
#include <gdiplus.h>
namespace wiki_review {
std::filesystem::path capture_path;
void capture(HWND window) {
    RECT rect{};GetWindowRect(window,&rect);
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    HDC source=GetWindowDC(window),dest=CreateCompatibleDC(source);
    HBITMAP bitmap=CreateCompatibleBitmap(source,width,height);
    HGDIOBJ previous=SelectObject(dest,bitmap);
    check(BitBlt(dest,0,0,width,height,source,0,0,SRCCOPY)!=0,"native menu capture");
    Gdiplus::GdiplusStartupInput startup;ULONG_PTR token{};
    check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"PNG startup");
    {
        Gdiplus::Bitmap image(bitmap,nullptr);
        const CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
        check(image.Save(capture_path.c_str(),&png,nullptr)==Gdiplus::Ok,"native PNG saved");
        Gdiplus::Bitmap decoded(capture_path.c_str());
        check(decoded.GetWidth()==static_cast<UINT>(width)&&decoded.GetHeight()==static_cast<UINT>(height),"PNG dimensions verified");
    }
    Gdiplus::GdiplusShutdown(token);SelectObject(dest,previous);DeleteObject(bitmap);DeleteDC(dest);ReleaseDC(window,source);
}
LRESULT CALLBACK host(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_COMMAND && LOWORD(wp)==ID_HELP_WIKI){++commands;return window_proc(window,msg,wp,lp);}
    return DefWindowProcW(window,msg,wp,lp);
}
void CALLBACK menu_input(HWND,UINT,UINT_PTR timer,DWORD) {
    KillTimer(nullptr,timer);
    HWND menu=FindWindowW(L"#32768",nullptr);DWORD pid{};GetWindowThreadProcessId(menu,&pid);
    if(!menu || pid!=GetCurrentProcessId()){EndMenu();return;}
    capture(menu);
    GUITHREADINFO info{};info.cbSize=sizeof(info);GetGUIThreadInfo(GetCurrentThreadId(),&info);
    if(info.hwndMenuOwner!=owner){EndMenu();return;}
    if(mouse) {
        RECT item{};if(!GetMenuItemRect(owner,popup,1,&item)){EndMenu();return;}
        SetCursorPos((item.left+item.right)/2,(item.top+item.bottom)/2);
        INPUT input[2]{};input[0].type=INPUT_MOUSE;input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
        input[1].type=INPUT_MOUSE;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
        SendInput(2,input,sizeof(INPUT));
    } else {
        // Native menu mnemonic selects and activates the SFX Wiki entry.
        INPUT input[2]{};input[0].type=INPUT_KEYBOARD;input[0].ki.wVk='S';
        input[1].type=INPUT_KEYBOARD;input[1].ki.wVk='S';input[1].ki.dwFlags=KEYEVENTF_KEYUP;
        SendInput(2,input,sizeof(INPUT));
    }
}
void CALLBACK menu_timeout(HWND,UINT,UINT_PTR timer,DWORD){KillTimer(nullptr,timer);EndMenu();}
}
int wmain(int argc,wchar_t** argv) {
    using namespace wiki_review;
    try {
        check(argc==2,"owned evidence directory supplied");
        const std::filesystem::path out=std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(out);
        root=out/L"Board with spaces";
        std::filesystem::create_directories(root/L"Docs/Sysop");
        std::ofstream(root/L"Docs/Sysop/wiki.html")<<"<title>Owned test</title>";
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=host;cls.lpszClassName=L"SfxHelpWikiOwnedTest";cls.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        check(RegisterClassW(&cls)!=0,"owned class registered");
        AppState state{};state.instance=cls.hInstance;
        owner=CreateWindowW(cls.lpszClassName,L"SFX Help menu focused check",WS_OVERLAPPEDWINDOW,100,100,520,200,nullptr,nullptr,cls.hInstance,nullptr);
        check(owner!=nullptr,"owned window created");state.window=owner;SetWindowLongPtrW(owner,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&state));
        popup=create_help_menu();HMENU bar=CreateMenu();AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(popup),L"&Help");SetMenu(owner,bar);
        check(GetMenuItemCount(popup)==2,"exactly two Help entries");
        check(GetMenuItemID(popup,0)==ID_HELP_ABOUT&&GetMenuItemID(popup,1)==ID_HELP_WIKI,"Wiki immediately follows About");
        wchar_t label[80]{};GetMenuStringW(popup,1,label,80,MF_BYPOSITION);
        check(std::wstring(label)==L"&SFX Wiki","exact label and native mnemonic");
        check((GetMenuState(popup,1,MF_BYPOSITION)&(MF_DISABLED|MF_GRAYED))==0,"Wiki enabled");
        ShowWindow(owner,SW_SHOWNORMAL);UpdateWindow(owner);SetForegroundWindow(owner);
        for(bool use_mouse:{false,true}) {
            mouse=use_mouse;capture_path=out/(mouse?L"help-mouse.png":L"help-keyboard.png");
            const auto before=commands;
            SetTimer(nullptr,1,250,menu_input);auto timeout=SetTimer(nullptr,2,2500,menu_timeout);
            POINT cursor{};GetCursorPos(&cursor);
            TrackPopupMenuEx(popup,TPM_LEFTALIGN|TPM_TOPALIGN,130,160,owner,nullptr);
            SetCursorPos(cursor.x,cursor.y);
            KillTimer(nullptr,timeout);
            MSG queued{};while(PeekMessageW(&queued,nullptr,0,0,PM_REMOVE)){TranslateMessage(&queued);DispatchMessageW(&queued);}
            check(commands==before+1,mouse?"native mouse activation reaches product handler":"native keyboard activation reaches product handler");
        }
        check(file==(root/L"Docs"/L"Sysop"/L"wiki.html").wstring()&&directory==(root/L"Docs"/L"Sysop").wstring()&&verb==L"open","selected root with spaces and normal file association");
        check(messages==0,"successful dispatch has no error");
        const auto original_cwd=std::filesystem::current_path();std::filesystem::current_path(out);
        open_sysop_wiki(owner);check(file==(root/L"Docs"/L"Sysop"/L"wiki.html").wstring(),"dispatch independent of working directory");
        std::filesystem::current_path(original_cwd);
        launch_result=31;open_sysop_wiki(owner);check(messages==1&&message.find(L"default app")!=std::wstring::npos,"association failure gives useful action");
        root=out/L"Different copied board";const auto before=launches;
        open_sysop_wiki(owner);check(launches==before&&message.find(L"missing")!=std::wstring::npos,"missing Wiki refuses launch and explains repair");
        std::filesystem::create_directories(root/L"Docs/Sysop/wiki.html");
        open_sysop_wiki(owner);check(launches==before,"directory masquerading as Wiki refused");
        std::filesystem::remove(root/L"Docs/Sysop/wiki.html");
        std::ofstream(root/L"Docs/Sysop/wiki.html")<<"<title>Second owned test</title>";
        launch_result=33;open_sysop_wiki(owner);check(file==(root/L"Docs"/L"Sysop"/L"wiki.html").wstring(),"different installation selected without cached root");
        throw_root=true;open_sysop_wiki(owner);check(message.find(L"could not locate")!=std::wstring::npos,"root exception has useful fallback");throw_root=false;
        SetWindowLongPtrW(owner,GWLP_USERDATA,0);DestroyWindow(owner);UnregisterClassW(cls.lpszClassName,cls.hInstance);
        std::cout<<"PASS "<<checks<<" checks; controlled launcher only; no browser or Runtime started\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
