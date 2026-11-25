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
void render(State& s,const std::filesystem::path& file) {
    RECT area{};GetClientRect(s.window,&area);const auto reference=GetDC(s.window);
    DoorPrintSurface target(reference,area.right,area.bottom);ReleaseDC(s.window,reference);
    FillRect(target.dc,&area,s.appearance.background_brush());
    door_print_children(s.window,s.window,target,area,s.appearance.background_brush());GdiFlush();
    // Independent native status paint must survive composition unchanged. This
    // detects the observed classic-theme controls leaking outside the page.
    const auto status=door_print_rect(s.status,s.window,false);RECT visible{};
    check(IntersectRect(&visible,&status,&area)!=FALSE,"status is inside render viewport");
    DoorPrintSurface expected(target.dc,status.right-status.left,status.bottom-status.top);
    door_print_control(s.status,expected,s.appearance.background_brush());
    const auto* actual=static_cast<const std::uint32_t*>(target.bits);
    const auto* correct=static_cast<const std::uint32_t*>(expected.bits);bool identical=true;
    for(int y=visible.top;y<visible.bottom;++y)for(int x=visible.left;x<visible.right;++x)
        identical=identical&&((actual[y*target.width+x]&0xffffffU)==
            (correct[(y-status.top)*expected.width+x-status.left]&0xffffffU));
    check(identical,"native page descendants cannot overwrite status pixels");
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=target.width;
    info.biHeight=-target.height;info.biPlanes=1;info.biBitCount=32;
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);
    header.bfSize=header.bfOffBits+static_cast<DWORD>(target.width*target.height*4);
    std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));
    out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(static_cast<const char*>(target.bits),static_cast<std::streamsize>(target.width)*target.height*4);
    check(out.good(),"write native ancestor-clipped render");
}
}
