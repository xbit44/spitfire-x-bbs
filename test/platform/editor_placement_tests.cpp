#include "sfx/platform/win32/editor_placement.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace win32 = sfx::platform::win32;
namespace {
unsigned checks{};
void require(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
bool same(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
}
int main() {
    const auto key = L"Software\\Spitfire-X BBS\\Tests\\EditorPlacement-" + std::to_wstring(GetCurrentProcessId());
    HWND window{};
    try {
        require(same(win32::visible_editor_rectangle({-1500,100,-600,800}, {-1920,40,0,1080},780,610),
            {-1500,100,-600,800}), "negative-coordinate second monitor is preserved");
        require(same(win32::visible_editor_rectangle({3000,2000,4500,3000}, {0,40,1280,1024},780,610),
            {0,40,1280,1024}), "removed monitor and oversized rectangle safely recover");
        require(same(win32::visible_editor_rectangle({0,0,100,100}, {0,40,600,480},780,610),
            {0,40,600,480}), "small work area remains usable");
        require(same(win32::visible_editor_rectangle({0,0,1,1}, {0,40,1920,1080},780,610),
            {0,40,780,650}), "minimum size respects top taskbar");
        win32::EditorPlacementStore store(key);
        require(!store.load(780,610), "missing preference is optional");
        window = CreateWindowW(L"STATIC", L"SFX placement test", WS_OVERLAPPEDWINDOW,
            70,80,900,680,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window != nullptr, "native test window");
        RECT actual{}; GetWindowRect(window,&actual);
        require(store.save(window), "normal position atomically saved");
        auto loaded = store.load(780,610);
        MONITORINFO info{sizeof(info)};
        require(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info)!=FALSE,
            "actual monitor work area");
        require(loaded && same(loaded->rectangle,win32::visible_editor_rectangle(actual,info.rcWork,780,610)) &&
            !loaded->maximized, "last monitor position and size restored independently");
        ShowWindow(window,SW_SHOWMAXIMIZED);
        require(store.save(window), "maximized state saved");
        loaded=store.load(780,610);
        require(loaded && loaded->maximized, "maximized state with normal rectangle restored");
        ShowWindow(window,SW_MINIMIZE);
        require(store.save(window) && store.load(780,610)->maximized,
            "minimization never persists invisible placement");
        HKEY registry{};
        require(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_QUERY_VALUE|KEY_SET_VALUE,&registry)==ERROR_SUCCESS,
            "exact disposable preference");
        DWORD size{},type{};
        RegQueryValueExW(registry,L"Placement",nullptr,&type,nullptr,&size);
        std::vector<BYTE> record(size);
        require(RegQueryValueExW(registry,L"Placement",nullptr,&type,record.data(),&size)==ERROR_SUCCESS,
            "read complete preference");
        // Corrupt the device name only: production must select an available display.
        require(size >= CCHDEVICENAME*sizeof(wchar_t), "record contains bounded monitor name");
        auto* device = reinterpret_cast<wchar_t*>(record.data()+size-CCHDEVICENAME*sizeof(wchar_t));
        for (int i=0;i<CCHDEVICENAME;++i) device[i]=i==0 ? L'?' : L'\0';
        require(RegSetValueExW(registry,L"Placement",0,REG_BINARY,record.data(),size)==ERROR_SUCCESS,
            "simulate removed display in exact test key");
        loaded=store.load(780,610);
        require(loaded && MonitorFromRect(&loaded->rectangle,MONITOR_DEFAULTTONULL)!=nullptr,
            "unknown display safely restores on an available monitor");
        record[0]=99;
        RegSetValueExW(registry,L"Placement",0,REG_BINARY,record.data(),size);
        require(!store.load(780,610), "unknown version rejected");
        RegCloseKey(registry);
        DestroyWindow(window); window=nullptr;
        require(RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str())==ERROR_SUCCESS,"remove only test preference");
        std::cout<<"PASS editor placement: "<<checks<<" checks\n";
        return 0;
    } catch(const std::exception& error) {
        if(window) DestroyWindow(window);
        RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str());
        std::cerr<<"FAIL editor placement: "<<error.what()<<'\n'; return 1;
    }
}
