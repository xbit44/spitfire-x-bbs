#pragma once
#include "sfx/platform/win32/desktop_theme.hpp"
#include <commctrl.h>
#include <algorithm>

namespace sfx::file_manager {
// FM-only native header fix. Custom-drawn items do not paint the unused
// trailing header area; leave other applications' appearance unchanged.
inline LRESULT CALLBACK file_header_paint(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,file_header_paint,id);
    const auto result=DefSubclassProc(window,message,w,l);
    if(message==WM_PAINT||message==WM_PRINTCLIENT){
        auto& appearance=*reinterpret_cast<platform::win32::DesktopAppearance*>(data);
        RECT blank{};GetClientRect(window,&blank);const auto count=Header_GetItemCount(window);
        int end{};for(int n=0;n<count;++n){RECT item{};if(Header_GetItemRect(window,n,&item))end=(std::max)(end,static_cast<int>(item.right));}
        blank.left=end;
        if(blank.left<blank.right){const auto dc=message==WM_PRINTCLIENT?reinterpret_cast<HDC>(w):GetDC(window);const auto brush=CreateSolidBrush(appearance.palette().surface);FillRect(dc,&blank,brush);DeleteObject(brush);if(message!=WM_PRINTCLIENT)ReleaseDC(window,dc);}
    }
    return result;
}
inline void apply_file_manager_visuals(HWND parent,platform::win32::DesktopAppearance& appearance){
    appearance.apply_controls(parent);
    EnumChildWindows(parent,[](HWND child,LPARAM data)->BOOL{wchar_t cls[64]{};GetClassNameW(child,cls,64);if(!_wcsicmp(cls,WC_HEADERW))SetWindowSubclass(child,file_header_paint,31,static_cast<DWORD_PTR>(data));return TRUE;},reinterpret_cast<LPARAM>(&appearance));
}
inline bool file_dialog_visual(HWND dialog,platform::win32::DesktopAppearance& appearance,UINT message,WPARAM w,LPARAM l,INT_PTR& result){
    if(message==WM_CTLCOLORDLG){result=reinterpret_cast<INT_PTR>(appearance.background_brush());return true;}
    if(message==WM_ERASEBKGND){RECT r{};GetClientRect(dialog,&r);FillRect(reinterpret_cast<HDC>(w),&r,appearance.background_brush());result=TRUE;return true;}
    LRESULT themed{};if(!appearance.control_message(dialog,message,w,l,themed))return false;
    // Dialog procedures return TRUE for handled WM_NOTIFY; the actual custom
    // draw result belongs in DWLP_MSGRESULT, not in the dialog return value.
    if(message==WM_NOTIFY){SetWindowLongPtrW(dialog,DWLP_MSGRESULT,themed);result=TRUE;}else result=themed;
    return true;
}
} // namespace sfx::file_manager
