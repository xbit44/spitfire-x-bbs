#include "sfx/platform/win32/desktop_theme.hpp"
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks{};void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
int expected_caption_inset(HFONT font,int height){
    LOGFONTW selected{};NONCLIENTMETRICSW system{};system.cbSize=sizeof(system);
    check(GetObjectW(font,sizeof(selected),&selected)&&SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(system),&system,0),"reference control and system fonts");
    const int ratio=std::abs(selected.lfHeight),base=std::abs(system.lfMessageFont.lfHeight);
    return std::min(MulDiv(GetSystemMetrics(SM_CXMENUCHECK),ratio,base),height)+MulDiv(5,ratio,base);
}
struct Surface {
    HDC dc{};HBITMAP bitmap{};HGDIOBJ prior{};void* pixels{};int width,height;
    Surface(int w,int h):width(w),height(h){dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);check(dc&&bitmap&&pixels,"owned native DIB");prior=SelectObject(dc,bitmap);}
    ~Surface(){SelectObject(dc,prior);DeleteObject(bitmap);DeleteDC(dc);}
    void save(const std::filesystem::path& path){const CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};Gdiplus::Bitmap image(width,height,width*4,PixelFormat32bppRGB,static_cast<BYTE*>(pixels));check(image.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok,"direct PNG evidence");Gdiplus::Bitmap decoded(path.c_str());check(decoded.GetLastStatus()==Gdiplus::Ok&&decoded.GetWidth()==static_cast<UINT>(width)&&decoded.GetHeight()==static_cast<UINT>(height),"PNG dimensions");}
};
void fill(HDC dc,RECT r,COLORREF color){const auto brush=CreateSolidBrush(color);FillRect(dc,&r,brush);DeleteObject(brush);}
bool same_caption(const Surface& a,const Surface& b,int left){const auto* p=static_cast<const DWORD*>(a.pixels);const auto* q=static_cast<const DWORD*>(b.pixels);for(int y=0;y<a.height;++y)for(int x=left;x<a.width;++x)if((p[y*a.width+x]&0xffffff)!=(q[y*a.width+x]&0xffffff))return false;return true;}
}
int main(int argc,char** argv){ULONG_PTR token{};try{
    check(argc==2,"new PNG evidence root or --auto required");const auto out=std::string_view(argv[1])=="--auto"?std::filesystem::temp_directory_path()/("SFX-checkbox-render-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())):std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(out),"retain previous evidence");std::filesystem::create_directories(out);
    Gdiplus::GdiplusStartupInput startup;check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"PNG encoder");INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native controls");
    sfx::platform::win32::DesktopAppearance appearance(L"Software\\Spitfire-X BBS\\Tests\\RC14-ReadOnly-Checkbox-Render");check(appearance.theme()==sfx::platform::win32::DesktopTheme::dark_retro,"new private unset preference uses dark theme");
    for(int dpi:{96,144,192}){
        const int width=MulDiv(500,dpi,96),height=MulDiv(30,dpi,96);RECT rect{0,0,width,height};
        const auto parent=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);check(parent!=nullptr,"owned native parent");
        const auto button=CreateWindowExW(0,L"BUTTON",L"Require Real Name on outgoing messages",WS_CHILD|BS_AUTOCHECKBOX,0,0,width,height,parent,nullptr,GetModuleHandleW(nullptr),nullptr);check(button!=nullptr,"actual checkbox");
        const auto font=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        const auto foreign=CreateFontW(-MulDiv(26,dpi,96),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Arial");check(font&&foreign,"different control and incoming DC fonts");SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        Surface actual(width,height),expected(width,height);SelectObject(actual.dc,foreign);const auto previous_font=GetCurrentObject(actual.dc,OBJ_FONT);SetTextColor(actual.dc,RGB(2,3,4));SetBkMode(actual.dc,OPAQUE);const auto old_color=GetTextColor(actual.dc);const auto old_mode=GetBkMode(actual.dc);
        NMCUSTOMDRAW draw{};draw.hdr.hwndFrom=button;draw.hdr.code=NM_CUSTOMDRAW;draw.dwDrawStage=CDDS_PREPAINT;draw.hdc=actual.dc;draw.rc=rect;LRESULT result{};check(appearance.control_message(parent,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&draw),result)&&result==CDRF_SKIPDEFAULT,"shared production checkbox custom draw");GdiFlush();
        fill(expected.dc,rect,appearance.palette().background);SelectObject(expected.dc,font);SetTextColor(expected.dc,appearance.palette().text);SetBkMode(expected.dc,TRANSPARENT);auto caption=rect;caption.left+=expected_caption_inset(font,height);DrawTextW(expected.dc,L"Require Real Name on outgoing messages",-1,&caption,DT_LEFT|DT_VCENTER|DT_SINGLELINE);GdiFlush();
        actual.save(out/("checkbox-"+std::to_string(dpi)+".png"));expected.save(out/("control-font-reference-"+std::to_string(dpi)+".png"));
        check(same_caption(actual,expected,caption.left),"caption pixels use actual control font, independent of incoming DC font");
        check(GetCurrentObject(actual.dc,OBJ_FONT)==previous_font&&GetTextColor(actual.dc)==old_color&&GetBkMode(actual.dc)==old_mode,"shared renderer restores drawing context");
        for(int i=0;i<25;++i)appearance.control_message(parent,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&draw),result);GdiFlush();check(same_caption(actual,expected,caption.left),"repeated repaint keeps exact clear caption pixels");
        std::vector<std::vector<DWORD>> boxes;
        for(int state=0;state<5;++state){
            SetWindowLongPtrW(button,GWL_STYLE,WS_CHILD|(state==4?BS_AUTO3STATE:BS_AUTOCHECKBOX));EnableWindow(button,state!=2);SendMessageW(button,BM_SETCHECK,state==4?BST_INDETERMINATE:state==1||state==2?BST_CHECKED:BST_UNCHECKED,0);draw.uItemState=state==3?CDIS_FOCUS:0;
            check(appearance.control_message(parent,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&draw),result),"state-aware shared draw");GdiFlush();fill(expected.dc,rect,appearance.palette().background);SelectObject(expected.dc,font);SetTextColor(expected.dc,state==2?appearance.palette().muted:appearance.palette().text);SetBkMode(expected.dc,TRANSPARENT);DrawTextW(expected.dc,L"Require Real Name on outgoing messages",-1,&caption,DT_LEFT|DT_VCENTER|DT_SINGLELINE);if(state==3){auto focus=caption;InflateRect(&focus,-2,-2);DrawFocusRect(expected.dc,&focus);}GdiFlush();
            check(same_caption(actual,expected,caption.left),"checked/disabled/focus/mixed captions keep intended font and color");
            check(GetCurrentObject(actual.dc,OBJ_FONT)==previous_font&&GetTextColor(actual.dc)==old_color&&GetBkMode(actual.dc)==old_mode,"every checkbox state restores caller DC");
            std::vector<DWORD> box;const auto* raw=static_cast<const DWORD*>(actual.pixels);for(int y=0;y<height;++y)for(int x=0;x<caption.left-5;++x)box.push_back(raw[y*width+x]&0xffffff);boxes.push_back(std::move(box));
            actual.save(out/("state-"+std::to_string(state)+"-"+std::to_string(dpi)+".png"));
        }
        check(boxes[0]!=boxes[1]&&boxes[1]!=boxes[4]&&boxes[0]!=boxes[4],"mixed indicator is visibly distinct from checked and unchecked");check(boxes[1]!=boxes[2],"disabled indicator remains distinct");
        DestroyWindow(button);DestroyWindow(parent);SelectObject(actual.dc,GetStockObject(SYSTEM_FONT));SelectObject(expected.dc,GetStockObject(SYSTEM_FONT));DeleteObject(font);DeleteObject(foreign);
    }
    Gdiplus::GdiplusShutdown(token);token=0;std::cout<<"PASS "<<checks<<" shared checkbox font/repaint checks\n";return 0;
}catch(const std::exception& e){if(token)Gdiplus::GdiplusShutdown(token);std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
