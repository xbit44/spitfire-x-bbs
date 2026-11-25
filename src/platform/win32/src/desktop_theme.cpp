#include "sfx/platform/win32/desktop_theme.hpp"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <utility>

namespace sfx::platform::win32 {
namespace {

void fill(HDC dc, const RECT& rect, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

bool is_class(HWND window, const wchar_t* expected) {
    wchar_t name[64]{};
    return GetClassNameW(window, name, 64) && _wcsicmp(name, expected) == 0;
}

LRESULT CALLBACK control_proc(HWND window, UINT message, WPARAM wparam,
    LPARAM lparam, UINT_PTR id, DWORD_PTR context) {
    auto* appearance = reinterpret_cast<DesktopAppearance*>(context);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, control_proc, id);
    // SetWindowTheme("", "") selects classic BUTTON rendering, which does not
    // send NM_CUSTOMDRAW. Own the paint entry point as well as the notification;
    // native BUTTON still owns input, checks, focus, accessibility and hit tests.
    if(is_class(window,L"BUTTON")) {
        const auto kind=GetWindowLongPtrW(window,GWL_STYLE)&BS_TYPEMASK;
        const bool painted=kind==BS_PUSHBUTTON||kind==BS_DEFPUSHBUTTON||kind==BS_CHECKBOX||
            kind==BS_AUTOCHECKBOX||kind==BS_3STATE||kind==BS_AUTO3STATE||kind==BS_RADIOBUTTON||kind==BS_AUTORADIOBUTTON;
        if(painted&&(message==WM_PAINT||message==WM_PRINTCLIENT)) {
            PAINTSTRUCT paint{};const auto dc=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wparam);
            if(dc){NMCUSTOMDRAW draw{};draw.hdr.hwndFrom=window;draw.hdr.code=NM_CUSTOMDRAW;
                draw.dwDrawStage=CDDS_PREPAINT;draw.hdc=dc;GetClientRect(window,&draw.rc);
                const auto state=SendMessageW(window,BM_GETSTATE,0,0);
                if(state&BST_PUSHED)draw.uItemState|=CDIS_SELECTED;
                if((state&BST_FOCUS)&&!(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))draw.uItemState|=CDIS_FOCUS;
                LRESULT ignored{};appearance->control_message(GetParent(window),WM_NOTIFY,0,reinterpret_cast<LPARAM>(&draw),ignored);
            }
            if(message==WM_PAINT)EndPaint(window,&paint);
            return 0;
        }
        if(painted&&(message==WM_ENABLE||message==WM_SETTEXT||message==WM_SETFONT||message==BM_SETCHECK||message==BM_SETSTATE||message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_UPDATEUISTATE)){
            const auto result=DefSubclassProc(window,message,wparam,lparam);InvalidateRect(window,nullptr,FALSE);return result;
        }
    }
    LRESULT result = 0;
    if (appearance->control_message(window, message, wparam, lparam, result)) return result;
    return DefSubclassProc(window, message, wparam, lparam);
}

BOOL CALLBACK apply_child(HWND child, LPARAM context) {
    auto* appearance = reinterpret_cast<DesktopAppearance*>(context);
    // Documented Win7 API; no private DarkMode_* theme names or ordinal imports.
    SetWindowTheme(child, appearance->theme() == DesktopTheme::dark_retro ? L"" : nullptr,
        appearance->theme() == DesktopTheme::dark_retro ? L"" : nullptr);
    SetWindowSubclass(child, control_proc, 1, reinterpret_cast<DWORD_PTR>(appearance));
    if (is_class(child, WC_LISTVIEWW)) {
        const auto& p = appearance->palette();
        ListView_SetBkColor(child, p.panel);
        ListView_SetTextBkColor(child, p.panel);
        ListView_SetTextColor(child, p.text);
    }
    if(is_class(child,WC_TREEVIEWW)) {
        const auto& p=appearance->palette();
        TreeView_SetBkColor(child,p.panel);
        TreeView_SetTextColor(child,p.text);
        TreeView_SetLineColor(child,p.border);
    }
    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
}

} // namespace

DesktopAppearance::DesktopAppearance(std::wstring key, bool soft_classic_content)
    : key_(std::move(key)), soft_classic_content_(soft_classic_content) {
    set_palette(DesktopTheme::dark_retro);
    (void)reload();
}

DesktopAppearance::~DesktopAppearance() {
    DeleteObject(background_);
    DeleteObject(panel_);
    DeleteObject(banner_);
}

void DesktopAppearance::set_palette(const DesktopTheme theme) {
    theme_ = theme;
    if (theme == DesktopTheme::dark_retro) {
        palette_ = {RGB(18,20,22), RGB(27,30,32), RGB(35,39,41), RGB(30,34,36),
            RGB(218,219,213), RGB(150,156,155), RGB(76,83,86), RGB(57,63,65),
            RGB(218,157,59), RGB(103,196,119), RGB(91,181,199), RGB(210,132,52),
            RGB(210,86,70), RGB(50,69,78), RGB(238,239,232), RGB(37,59,104),
            RGB(27,30,32), RGB(218,219,213), RGB(218,157,59)};
    } else {
        // Keep Config's accepted Classic appearance. Control Panel opts into
        // softer content with its former stripe gray as the base.
        palette_ = {RGB(226,229,232), RGB(249,249,247), RGB(235,237,237), RGB(240,240,240),
            RGB(24,27,30), RGB(84,90,94), RGB(133,141,147), RGB(210,215,219),
            RGB(132,82,0), RGB(24,111,43), RGB(0,99,124), RGB(147,78,0),
            RGB(164,35,35), RGB(211,228,246), RGB(15,35,55), RGB(222,234,250),
            RGB(27,42,52), RGB(228,233,235), RGB(255,192,64)};
        if (soft_classic_content_) {
            palette_.panel = RGB(240,240,240);
            palette_.alternate = RGB(224,224,224);
            palette_.cpu = palette_.surface;
        }
    }
    const HBRUSH background = CreateSolidBrush(palette_.background);
    const HBRUSH panel = CreateSolidBrush(palette_.panel);
    DeleteObject(background_);
    DeleteObject(panel_);
    background_ = background;
    panel_ = panel;
    DeleteObject(banner_);
    banner_ = CreateSolidBrush(palette_.banner);
}

bool DesktopAppearance::reload() {
    DWORD value = static_cast<DWORD>(DesktopTheme::dark_retro);
    DWORD type = 0, bytes = sizeof(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        const auto status = RegQueryValueExW(key, L"Theme", nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(value) || value > 1)
            value = static_cast<DWORD>(DesktopTheme::dark_retro);
    }
    const auto next = static_cast<DesktopTheme>(value);
    if (next == theme_) return false;
    set_palette(next);
    return true;
}

UINT DesktopAppearance::changed_message() {
    static const UINT value = RegisterWindowMessageW(L"SFX.DesktopThemeChanged.v1");
    return value;
}

bool DesktopAppearance::save(const DesktopTheme theme) {
    if (theme != DesktopTheme::classic && theme != DesktopTheme::dark_retro) return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = static_cast<DWORD>(theme);
    bool ok = RegSetValueExW(key, L"Theme", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    DWORD actual = 9, type = 0, bytes = sizeof(actual);
    ok = ok && RegQueryValueExW(key, L"Theme", nullptr, &type,
        reinterpret_cast<BYTE*>(&actual), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(actual) && actual == value;
    RegCloseKey(key);
    if (!ok) return false;
    set_palette(theme);
    const UINT notification = changed_message();
    if (notification != 0) PostMessageW(HWND_BROADCAST, notification, 0, 0);
    return true;
}

HMENU DesktopAppearance::append_menu(HMENU view) const {
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, classic_command, L"&Classic");
    AppendMenuW(menu, MF_STRING, dark_command, L"&Dark Retro");
    check_menu(menu);
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(menu), L"&Theme");
    return menu;
}

void DesktopAppearance::check_menu(HMENU menu) const {
    CheckMenuRadioItem(menu, classic_command, dark_command,
        theme_ == DesktopTheme::classic ? classic_command : dark_command, MF_BYCOMMAND);
}

void DesktopAppearance::apply_controls(HWND parent) {
    EnumChildWindows(parent, apply_child, reinterpret_cast<LPARAM>(this));
    RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

bool DesktopAppearance::control_message(HWND, UINT message, WPARAM wparam,
    LPARAM lparam, LRESULT& result) const {
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
            message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN) {
        const auto child = reinterpret_cast<HWND>(lparam);
        const bool input = is_class(child, L"EDIT") || is_class(child, L"LISTBOX") ||
            is_class(child, L"ComboLBox");
        const auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, IsWindowEnabled(child) ? palette_.text : palette_.muted);
        SetBkColor(dc, input ? palette_.panel : palette_.background);
        result = reinterpret_cast<LRESULT>(input ? panel_ : background_);
        return true;
    }
    if (message == WM_DRAWITEM) {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (!draw || (draw->CtlType != ODT_COMBOBOX && draw->CtlType != ODT_LISTBOX)) return false;
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        fill(draw->hDC, draw->rcItem, selected ? palette_.selection : palette_.panel);
        if (draw->itemID != static_cast<UINT>(-1)) {
            std::array<wchar_t, 1024> label{};
            const UINT length_message = draw->CtlType == ODT_COMBOBOX ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
            const UINT text_message = draw->CtlType == ODT_COMBOBOX ? CB_GETLBTEXT : LB_GETTEXT;
            const auto length = SendMessageW(draw->hwndItem, length_message, draw->itemID, 0);
            if (length >= 0 && length < static_cast<LRESULT>(label.size())) {
                SendMessageW(draw->hwndItem, text_message, draw->itemID,
                    reinterpret_cast<LPARAM>(label.data()));
                SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED) ? palette_.muted :
                    selected ? palette_.selected_text : palette_.text);
                SetBkMode(draw->hDC, TRANSPARENT);
                RECT rect = draw->rcItem; rect.left += 3;
                DrawTextW(draw->hDC, label.data(), -1, &rect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
        if ((draw->itemState & ODS_FOCUS) && !(draw->itemState & ODS_NOFOCUSRECT))
            DrawFocusRect(draw->hDC, &draw->rcItem);
        result = TRUE;
        return true;
    }
    if (message == WM_MEASUREITEM) {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (item && (item->CtlType == ODT_COMBOBOX || item->CtlType == ODT_LISTBOX)) {
            const HDC screen = GetDC(nullptr);
            const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
            if (screen) ReleaseDC(nullptr, screen);
            item->itemHeight = static_cast<UINT>(MulDiv(19, dpi, 96));
            result = TRUE;
            return true;
        }
    }
    if (message == WM_NOTIFY) {
        auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
        if (!draw || draw->hdr.code != NM_CUSTOMDRAW) return false;
        if (theme_ == DesktopTheme::dark_retro && is_class(draw->hdr.hwndFrom, WC_HEADERW)) {
            if (draw->dwDrawStage == CDDS_PREPAINT) { result = CDRF_NOTIFYITEMDRAW; return true; }
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                fill(draw->hdc, draw->rc, palette_.surface);
                std::array<wchar_t, 256> label{};
                HDITEMW item{}; item.mask = HDI_TEXT; item.pszText = label.data(); item.cchTextMax = 256;
                Header_GetItem(draw->hdr.hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
                SetTextColor(draw->hdc, palette_.gold); SetBkMode(draw->hdc, TRANSPARENT);
                RECT rect = draw->rc; rect.left += 5;
                DrawTextW(draw->hdc, label.data(), -1, &rect,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                result = CDRF_SKIPDEFAULT;
                return true;
            }
        }
        if (is_class(draw->hdr.hwndFrom, L"BUTTON") && draw->dwDrawStage == CDDS_PREPAINT) {
            const HWND button = draw->hdr.hwndFrom;
            const auto style = GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK;
            if (style == BS_OWNERDRAW) return false;
            const int saved_dc=SaveDC(draw->hdc);
            if(!saved_dc)return false;
            const auto font=reinterpret_cast<HFONT>(SendMessageW(button,WM_GETFONT,0,0));
            if(font)SelectObject(draw->hdc,font);
            const bool three_state=style==BS_3STATE||style==BS_AUTO3STATE;
            const bool radio=style==BS_RADIOBUTTON||style==BS_AUTORADIOBUTTON;
            const bool check = style == BS_AUTOCHECKBOX || style == BS_CHECKBOX || three_state || radio;
            if(!check&&style!=BS_PUSHBUTTON&&style!=BS_DEFPUSHBUTTON){RestoreDC(draw->hdc,saved_dc);return false;}
            const bool disabled = !IsWindowEnabled(button);
            const bool pressed = (draw->uItemState & CDIS_SELECTED) != 0;
            fill(draw->hdc, draw->rc, check ? palette_.background :
                pressed ? palette_.selection : palette_.surface);
            RECT rect = draw->rc;
            if (check) {
                // GetSystemMetrics uses the system DPI. Match the assigned
                // control font when a window has been scaled independently;
                // this also works on Win7 without newer DPI-only imports.
                NONCLIENTMETRICSW metrics{};metrics.cbSize=sizeof(metrics);LOGFONTW assigned{};
                int numerator=1,denominator=1;
                if(font&&GetObjectW(font,sizeof(assigned),&assigned)&&
                    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0)&&
                    assigned.lfHeight&&metrics.lfMessageFont.lfHeight){
                    numerator=std::abs(assigned.lfHeight);denominator=std::abs(metrics.lfMessageFont.lfHeight);
                }
                const int size = std::max(1,std::min<int>(MulDiv(GetSystemMetrics(SM_CXMENUCHECK),numerator,denominator), rect.bottom - rect.top));
                RECT box{rect.left, rect.top + (rect.bottom - rect.top - size) / 2,
                    rect.left + size, rect.top + (rect.bottom - rect.top + size) / 2};
                const auto state=SendMessageW(button,BM_GETCHECK,0,0);
                const bool mixed=three_state&&state==BST_INDETERMINATE;
                DrawFrameControl(draw->hdc, &box, DFC_BUTTON, (radio?DFCS_BUTTONRADIO:mixed?DFCS_BUTTON3STATE:DFCS_BUTTONCHECK) |
                    (state == BST_CHECKED || mixed ? DFCS_CHECKED : 0) |
                    (disabled ? DFCS_INACTIVE : 0));
                rect.left += size + std::max(1,MulDiv(5,numerator,denominator));
            } else {
                if(theme_==DesktopTheme::classic)DrawFrameControl(draw->hdc,&rect,DFC_BUTTON,DFCS_BUTTONPUSH|(pressed?DFCS_PUSHED:0)|(disabled?DFCS_INACTIVE:0));
                else {const HBRUSH border = CreateSolidBrush(disabled ? palette_.border : palette_.gold);
                    FrameRect(draw->hdc, &rect, border); DeleteObject(border);}
                InflateRect(&rect, -4, -2);
            }
            std::array<wchar_t, 512> caption{};
            GetWindowTextW(button, caption.data(), static_cast<int>(caption.size()));
            SetTextColor(draw->hdc, disabled ? palette_.muted : palette_.text);
            SetBkMode(draw->hdc, TRANSPARENT);
            const auto ui=SendMessageW(button,WM_QUERYUISTATE,0,0);
            DrawTextW(draw->hdc, caption.data(), -1, &rect,
                DT_VCENTER | DT_SINGLELINE | (check ? DT_LEFT : DT_CENTER) | ((ui&UISF_HIDEACCEL)?DT_HIDEPREFIX:0));
            if (draw->uItemState & CDIS_FOCUS) {
                InflateRect(&rect, -2, -2); DrawFocusRect(draw->hdc, &rect);
            }
            RestoreDC(draw->hdc,saved_dc);
            result = CDRF_SKIPDEFAULT;
            return true;
        }
    }
    return false;
}

} // namespace sfx::platform::win32
