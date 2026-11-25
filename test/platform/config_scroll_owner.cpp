// Owned synthetic on-screen pixels: no installed configuration or remote BBS.
#define main retained_scroll_fixture_main
#define render retained_scroll_render
#include "door_command_line_early.cpp"
#undef render
#undef main
#include "door_native_render.hpp"

namespace {
void scroll_pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}GdiFlush();}
std::vector<std::uint32_t> screen_pixels(HWND window,const std::filesystem::path& file){
    RECT r{};GetClientRect(window,&r);const auto ref=GetDC(window);DoorPrintSurface image(ref,r.right,r.bottom);
    check(BitBlt(image.dc,0,0,r.right,r.bottom,ref,0,0,SRCCOPY)!=FALSE,"capture owned window pixels");ReleaseDC(window,ref);GdiFlush();
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=r.right;info.biHeight=-r.bottom;info.biPlanes=1;info.biBitCount=32;
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);header.bfSize=header.bfOffBits+static_cast<DWORD>(r.right*r.bottom*4);
    std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&info),sizeof(info));out.write(static_cast<const char*>(image.bits),static_cast<std::streamsize>(r.right)*r.bottom*4);check(out.good(),"save actual owned pixels");
    const auto* p=static_cast<const std::uint32_t*>(image.bits);return {p,p+static_cast<std::size_t>(r.right)*r.bottom};
}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh output directory");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve previous captures");std::filesystem::create_directories(output);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native classes");
    State s;s.dpi=96;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\owner-scroll-synthetic-not-installed";
    s.window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"STATIC",L"SFX owned scroll diagnostic",WS_POPUP|WS_CLIPCHILDREN,20,20,840,600,nullptr,nullptr,s.instance,nullptr);
    check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"owned native controls");
    door_catalog_fixture=snapshot;door_change_fixture=change;seed();s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);
    std::size_t damaged{};unsigned cases{};
    for(std::size_t page_index=0;page_index<s.pages.size();++page_index){
        if(!s.pages[page_index])continue;const int page_id=static_cast<int>(page_index);
        show_page(s,page_id);
        if(page_id==26){s.doors.menu=2;s.doors.door=17;s.doors.entry=42;refresh_door_configuration_page(s);load_door_fields(s);door_show_view(s,DoorPage::settings);}
        layout(s,840,600);RedrawWindow(s.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);scroll_pump();
        const auto page=s.pages[page_id];
        for(int step=0;step<11;++step){
            const short delta=step<5?-WHEEL_DELTA:WHEEL_DELTA;
            if(step<8)SendMessageW(page,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(delta)),0);
            else if(step<10)SendMessageW(page,WM_VSCROLL,step==8?SB_BOTTOM:SB_TOP,0);
            else {HWND last{};for(HWND h=GetWindow(page,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT))if(IsWindowVisible(h)&&IsWindowEnabled(h)&&(GetWindowLongPtrW(h,GWL_STYLE)&WS_TABSTOP))last=h;if(last){SetFocus(last);HideCaret(last);}}
            scroll_pump();
            const auto prefix=std::to_string(page_id)+"-"+std::to_string(step);
            const auto actual=screen_pixels(page,output/(prefix+"-scrolled.bmp"));
            RedrawWindow(page,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);scroll_pump();
            const auto repaired=screen_pixels(page,output/(prefix+"-redrawn.bmp"));
            std::size_t different{};for(std::size_t i=0;i<actual.size();++i)if((actual[i]&0xffffffU)!=(repaired[i]&0xffffffU))++different;
            std::cout<<"page="<<page_id<<" step="<<step<<" offset="<<GetScrollPos(page,SB_VERT)<<" stale_pixels="<<different<<"\n";damaged+=different;++cases;
        }
    }
    DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);door_catalog_fixture=nullptr;door_change_fixture=nullptr;
    std::cout<<"RESULT "<<cases<<" actual-window scroll comparisons; stale_pixels="<<damaged<<"\n";return damaged?2:0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
