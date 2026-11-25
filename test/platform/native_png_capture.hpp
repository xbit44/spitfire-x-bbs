#pragma once
#include <gdiplus.h>
#pragma comment(lib,"gdiplus.lib")

// Diagnostic only. Each native WM_PRINT gets its own DC: themed controls can
// alter that DC's clipping. Composite only the visible ancestor-client region.
// This captures actual controls; it does not repaint or repair their pixels.
namespace {
struct DoorPrintSurface {
    HDC dc{}; HBITMAP bitmap{}; HGDIOBJ previous{}; void* bits{};
    int width{},height{};
    DoorPrintSurface(HDC reference,int w,int h):width(w),height(h) {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        dc=CreateCompatibleDC(reference);
        bitmap=CreateDIBSection(reference,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc||!bitmap)throw std::runtime_error("native print surface unavailable");
        previous=SelectObject(dc,bitmap);
    }
    ~DoorPrintSurface(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
    DoorPrintSurface(const DoorPrintSurface&)=delete;
    DoorPrintSurface& operator=(const DoorPrintSurface&)=delete;
};
RECT door_print_rect(HWND child,HWND root,bool client) {
    RECT area{};
    if(client){GetClientRect(child,&area);MapWindowPoints(child,root,reinterpret_cast<POINT*>(&area),2);}
    else {GetWindowRect(child,&area);MapWindowPoints(HWND_DESKTOP,root,reinterpret_cast<POINT*>(&area),2);}
    return area;
}
void door_print_control(HWND child,DoorPrintSurface& surface,HBRUSH background) {
    const RECT area{0,0,surface.width,surface.height};FillRect(surface.dc,&area,background);
    const int saved=SaveDC(surface.dc);
    SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(surface.dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
    RestoreDC(surface.dc,saved);GdiFlush();
}
void door_print_children(HWND parent,HWND root,DoorPrintSurface& target,const RECT& viewport,HBRUSH background) {
    for(auto child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        const auto area=door_print_rect(child,root,false);RECT visible{};
        if(!IntersectRect(&visible,&area,&viewport))continue;
        DoorPrintSurface local(target.dc,area.right-area.left,area.bottom-area.top);
        door_print_control(child,local,background);
        if(!BitBlt(target.dc,visible.left,visible.top,visible.right-visible.left,visible.bottom-visible.top,
            local.dc,visible.left-area.left,visible.top-area.top,SRCCOPY))throw std::runtime_error("native composite failed");
        const auto client=door_print_rect(child,root,true);RECT nested{};
        if(IntersectRect(&nested,&client,&viewport))door_print_children(child,root,target,nested,background);
    }
}

}

// Encode the in-memory native render directly. No on-disk bitmap intermediate.
void render_native_png(HWND window,HBRUSH background,const std::filesystem::path& file){
    RECT rect{};GetClientRect(window,&rect);const int width=rect.right,height=rect.bottom;
    const auto reference=GetDC(window);DoorPrintSurface surface(reference,width,height);ReleaseDC(window,reference);const auto bits=surface.bits;
    FillRect(surface.dc,&rect,background);door_print_children(window,window,surface,rect,background);GdiFlush();
    Gdiplus::GdiplusStartupInput startup;ULONG_PTR token{};check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"PNG encoder startup");
    {
        const CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
        Gdiplus::Bitmap image(width,height,width*4,PixelFormat32bppRGB,static_cast<BYTE*>(bits));
        check(image.Save(file.c_str(),&png,nullptr)==Gdiplus::Ok,"lossless PNG saved");
        Gdiplus::Bitmap decoded(file.c_str());check(decoded.GetLastStatus()==Gdiplus::Ok&&decoded.GetWidth()==static_cast<UINT>(width)&&decoded.GetHeight()==static_cast<UINT>(height),"PNG decoded dimensions");
        Gdiplus::Rect area(0,0,width,height);Gdiplus::BitmapData data{};check(decoded.LockBits(&area,Gdiplus::ImageLockModeRead,PixelFormat32bppRGB,&data)==Gdiplus::Ok,"PNG decoded pixels");
        bool same=true;for(int y=0;y<height&&same;++y){const auto* actual=static_cast<const BYTE*>(data.Scan0)+y*data.Stride;const auto* expected=static_cast<const BYTE*>(bits)+y*width*4;
            for(int x=0;x<width&&same;++x)for(int c=0;c<3;++c)if(actual[x*4+c]!=expected[x*4+c])same=false;}
        decoded.UnlockBits(&data);check(same,"PNG lossless RGB equality");
    }
    Gdiplus::GdiplusShutdown(token);
}
void render_png(State& s,const std::filesystem::path& file){render_native_png(s.window,s.appearance.background_brush(),file);}
