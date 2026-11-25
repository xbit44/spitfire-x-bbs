// Offscreen native rendering of the shipping draw_monitor; no Runtime or
// installation is started and no UI preferences are changed.
#define wWinMain unused_product_entry
#include "../../src/apps/control_panel/main.cpp"
#undef wWinMain
#include <fstream>
#include <iostream>

int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2) throw std::runtime_error("fresh evidence output required");
        const std::filesystem::path output=argv[1];
        if(std::filesystem::exists(output)) throw std::runtime_error("preserve original render");
        std::filesystem::create_directories(output);
        for(UINT dpi : {96U,144U,192U}) for(int compact : {0,1}) for(bool gray : {false,true}) {
            g_automation_dpi=dpi;
            const int width=MulDiv(251,dpi,96),height=MulDiv(compact?285:399,dpi,96);
            HWND window=CreateWindowW(L"STATIC",L"",WS_POPUP,0,0,width,height,nullptr,nullptr,nullptr,nullptr);
            if(!window) throw std::runtime_error("hidden offscreen native canvas");
            AppState state;state.preferences.system_monitor_grayscale=gray;
            state.ui_font=CreateFontW(-MulDiv(16,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");
            state.panel_brush=CreateSolidBrush(kPanelColor);
            for(int level : {0,16384,262144,3145728,25165824,134217728}) {
                state.disk_history=app::BoundedMetricHistory(60);
                for(int i=0;i<60;++i) state.disk_history.push(i%12<3?0:level);
                state.metrics.storage_activity_available=true;
                state.metrics.storage_read_bytes_per_second=level*0.75;
                state.metrics.storage_write_bytes_per_second=level*0.25;
                const auto name=std::to_wstring(dpi)+L"-"+std::to_wstring(compact)+L"-"+(gray?L"gray":L"color")+L"-"+std::to_wstring(level);
                HDC screen=GetDC(window),dc=CreateCompatibleDC(screen);
                HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);auto old=SelectObject(dc,bitmap);
                draw_monitor(window,state,dc);
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
                info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*4);
                SelectObject(dc,old);
                if(!GetDIBits(dc,bitmap,0,static_cast<UINT>(height),pixels.data(),&info,DIB_RGB_COLORS)) throw std::runtime_error("native bitmap readback");
                BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
                header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
                std::ofstream image(output/(name+L".bmp"),std::ios::binary);
                image.write(reinterpret_cast<const char*>(&header),sizeof(header));image.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
                image.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
                std::ofstream(output/(name+L".tsv"))<<state.automation.monitor_content;
                DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
            }
            DeleteObject(state.ui_font);DeleteObject(state.panel_brush);DestroyWindow(window);
        }
        std::cout<<"PASS 72 offscreen shipping monitor renders; 96/144/192 DPI, compact/normal, color/gray, idle/low/moderate/full\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
