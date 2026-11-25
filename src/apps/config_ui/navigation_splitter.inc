// Per-operator/per-installation layout preference, in logical96-DPI units.
// It never changes BBS configuration, caller policy or a different operator.
void layout(State&,int,int);
#ifdef SFX_ALPHA_TEST_AUTOMATION
std::wstring navigation_registry_fixture;
#endif
std::wstring navigation_registry(const State& s){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(!navigation_registry_fixture.empty())return navigation_registry_fixture;
#endif
    return operator_registry_key(s.root);
}
void load_navigation_width(State& s){
    HKEY key{};if(RegOpenKeyExW(HKEY_CURRENT_USER,navigation_registry(s).c_str(),0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return;
    DWORD value{},size=sizeof(value),type{};
    if(RegQueryValueExW(key,L"NavigationWidth96",nullptr,&type,reinterpret_cast<BYTE*>(&value),&size)==ERROR_SUCCESS&&type==REG_DWORD&&size==sizeof(value)&&value>=140&&value<=640)s.navigation_width=static_cast<int>(value);
    RegCloseKey(key);
}
void save_navigation_width(State& s){
    HKEY key{};if(RegCreateKeyExW(HKEY_CURRENT_USER,navigation_registry(s).c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return;
    const auto value=static_cast<DWORD>(s.navigation_width);
    RegSetValueExW(key,L"NavigationWidth96",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));RegCloseKey(key);
}
bool navigation_divider_hit(const State& s,POINT p){
    if(!s.navigation||!IsWindowVisible(s.navigation))return false;
    RECT r{};GetWindowRect(s.navigation,&r);MapWindowPoints(HWND_DESKTOP,s.window,reinterpret_cast<POINT*>(&r),2);
    return p.x>=r.right&&p.x<r.right+MulDiv(12,s.dpi,96)&&p.y>=r.top&&p.y<r.bottom;
}
std::optional<LRESULT> navigation_splitter_message(State& s,UINT message,WPARAM w,LPARAM l){
    const auto cancel=[&]{s.resizing_navigation=false;s.navigation_width=s.navigation_width_start;ReleaseCapture();RECT r{};GetClientRect(s.window,&r);layout(s,r.right,r.bottom);};
    if(message==WM_SETCURSOR){POINT p{};GetCursorPos(&p);ScreenToClient(s.window,&p);if(s.resizing_navigation||navigation_divider_hit(s,p)){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}}
    if(message==WM_LBUTTONDOWN&&navigation_divider_hit(s,{GET_X_LPARAM(l),GET_Y_LPARAM(l)})){
        s.navigation_width_start=s.navigation_width;s.resizing_navigation=true;SetCapture(s.window);return 0;
    }
    if(message==WM_MOUSEMOVE&&s.resizing_navigation){
        RECT r{};GetClientRect(s.window,&r);
        const auto value=std::clamp(MulDiv(GET_X_LPARAM(l),96,s.dpi)-12,140,std::max(140,std::min(640,MulDiv(r.right,96,s.dpi)-554)));
        if(value!=s.navigation_width){s.navigation_width=value;layout(s,r.right,r.bottom);}return 0;
    }
    if(message==WM_LBUTTONUP&&s.resizing_navigation){s.resizing_navigation=false;ReleaseCapture();save_navigation_width(s);return 0;}
    if(s.resizing_navigation&&(message==WM_CANCELMODE||message==WM_CAPTURECHANGED||(message==WM_KEYDOWN&&w==VK_ESCAPE))){cancel();return 0;}
    return {};
}
