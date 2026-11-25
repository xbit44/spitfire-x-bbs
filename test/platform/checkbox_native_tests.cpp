// Exercise native BUTTON dispatch, including the disabled/classic fallback.
// The older synthetic NM_CUSTOMDRAW test cannot detect omitted notifications.
#define main retained_synthetic_checkbox_main
#include "checkbox_render_tests.cpp"
#undef main
#include <uxtheme.h>
#pragma comment(lib,"uxtheme.lib")
namespace {
unsigned paints{},enables{},notifications{};
LRESULT CALLBACK native_root(HWND h,UINT m,WPARAM w,LPARAM l){
    auto* a=reinterpret_cast<sfx::platform::win32::DesktopAppearance*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NOTIFY&&reinterpret_cast<NMHDR*>(l)->code==NM_CUSTOMDRAW)++notifications;
    LRESULT r{};if(a&&a->control_message(h,m,w,l,r))return r;
    return DefWindowProcW(h,m,w,l);
}
LRESULT CALLBACK trace_button(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR){
    if(m==WM_PAINT)++paints;if(m==WM_ENABLE)++enables;
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,trace_button,id);
    return DefSubclassProc(h,m,w,l);
}
}
int main(int argc,char** argv){ULONG_PTR token{};try{
    check(argc==2||argc==3,"fresh evidence path and optional classic fallback required");const bool forced_classic=argc==3;const auto out=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(out),"preserve failed evidence");std::filesystem::create_directories(out);
    Gdiplus::GdiplusStartupInput startup;check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"PNG encoder");
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native controls");
    WNDCLASSW wc{};wc.lpfnWndProc=native_root;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SFXOwnedNativeCheckboxRegression";check(RegisterClassW(&wc)!=0,"owned class");
    sfx::platform::win32::DesktopAppearance a(L"Software\\Spitfire-X BBS\\Tests\\RC8NativeCheckbox");
    unsigned failures{};
    for(int dpi:{96,144,192})for(int theme:{1,0}){if(forced_classic&&(dpi!=96||theme!=0))continue;
        check(a.save(static_cast<sfx::platform::win32::DesktopTheme>(theme)),"isolated theme preference");
        const int width=MulDiv(430,dpi,96),height=MulDiv(28,dpi,96);RECT rect{0,0,width,height};
        const auto parent=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,wc.lpszClassName,L"SFX checkbox qualification",WS_POPUP,20,20,width,height,nullptr,nullptr,wc.hInstance,nullptr);check(parent!=nullptr,"native root");SetWindowLongPtrW(parent,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&a));
        const auto button=CreateWindowExW(0,L"BUTTON",L"Start BBS when Windows starts",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,width,height,parent,nullptr,wc.hInstance,nullptr);
        check(button&&SetWindowSubclass(button,trace_button,9,0),"native button dispatch");
        const auto font=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);a.apply_controls(parent);if(forced_classic)SetWindowTheme(button,L"",L"");RemoveWindowSubclass(button,trace_button,9);check(SetWindowSubclass(button,trace_button,9,0)!=FALSE,"outer paint observer");ShowWindow(parent,SW_SHOWNOACTIVATE);
        for(int state:{0,1,0,1}){
            EnableWindow(button,state);SendMessageW(button,BM_SETCHECK,BST_UNCHECKED,0);RedrawWindow(parent,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            Surface actual(width,height),expected(width,height);const auto painted=GetDC(button);check(painted&&BitBlt(actual.dc,0,0,width,height,painted,0,0,SRCCOPY),"actual WM_PAINT control pixels");ReleaseDC(button,painted);
            fill(expected.dc,rect,a.palette().background);SelectObject(expected.dc,font);SetTextColor(expected.dc,state?a.palette().text:a.palette().muted);SetBkMode(expected.dc,TRANSPARENT);
            RECT caption=rect;caption.left+=expected_caption_inset(font,height);
            DrawTextW(expected.dc,L"Start BBS when Windows starts",-1,&caption,DT_LEFT|DT_VCENTER|DT_SINGLELINE);GdiFlush();
            const auto name=std::to_string(dpi)+"-theme"+std::to_string(theme)+"-enabled"+std::to_string(state)+"-paint"+std::to_string(paints);
            actual.save(out/(name+".png"));expected.save(out/(name+"-reference.png"));
            const bool matches=same_caption(actual,expected,caption.left);if(!matches)++failures;
            std::cout<<name<<" caption_exact="<<matches<<" native_notifications="<<notifications<<'\n';
        }
        DestroyWindow(parent);DeleteObject(font);
    }
    check(paints>0&&enables>0,"actual WM_PAINT and WM_ENABLE dispatched");
    Gdiplus::GdiplusShutdown(token);token=0;
    std::cout<<"caption_failures="<<failures<<" paints="<<paints<<" enables="<<enables<<" notifications="<<notifications<<'\n';
    return failures?2:0;
}catch(const std::exception& e){if(token)Gdiplus::GdiplusShutdown(token);std::cerr<<e.what()<<'\n';return 1;}}
