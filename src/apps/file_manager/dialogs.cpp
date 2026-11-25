#include "dialogs.hpp"
#include "visuals.hpp"
#include <shobjidl.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
namespace sfx::file_manager {
namespace {
struct ReadOnlyText {platform::win32::DesktopAppearance& appearance;const std::wstring& title;const std::wstring& body;bool confirmation;HFONT font{};int dpi{96};};
void readonly_layout(HWND dialog,ReadOnlyText& c){RECT r{};GetClientRect(dialog,&r);const auto s=[&](int n){return MulDiv(n,c.dpi,96);};MoveWindow(GetDlgItem(dialog,101),s(12),s(12),(std::max)(s(80),static_cast<int>(r.right)-s(24)),(std::max)(s(60),static_cast<int>(r.bottom)-s(64)),TRUE);MoveWindow(GetDlgItem(dialog,IDOK),r.right-s(c.confirmation?224:116),r.bottom-s(40),s(100),s(28),TRUE);if(c.confirmation)MoveWindow(GetDlgItem(dialog,IDCANCEL),r.right-s(112),r.bottom-s(40),s(100),s(28),TRUE);}
INT_PTR CALLBACK readonly_proc(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam){
    auto* context=reinterpret_cast<ReadOnlyText*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG){context=reinterpret_cast<ReadOnlyText*>(lparam);SetWindowLongPtrW(dialog,DWLP_USER,lparam);SetWindowTextW(dialog,context->title.c_str());
        NONCLIENTMETRICSW metrics{sizeof(metrics)};SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0);context->font=CreateFontIndirectW(&metrics.lfMessageFont);
        const auto dc=GetDC(dialog);if(dc){context->dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(dialog,dc);}if(context->dpi<=0)context->dpi=96;
        auto edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",native_form_lines(context->body).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_HSCROLL,12,12,660,400,dialog,reinterpret_cast<HMENU>(101),nullptr,nullptr);SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);
        for(const auto id:{IDOK,IDCANCEL}){if(id==IDCANCEL&&!context->confirmation)continue;auto button=CreateWindowW(L"BUTTON",id==IDOK?(context->confirmation?L"Yes":L"Close"):L"No",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,id==IDOK?462:572,424,100,28,dialog,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),nullptr,nullptr);SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);}
        SendMessageW(edit,EM_SETLIMITTEXT,8*1024*1024,0);apply_file_manager_visuals(dialog,context->appearance);readonly_layout(dialog,*context);SetFocus(GetDlgItem(dialog,context->confirmation?IDCANCEL:IDOK));return FALSE;}
    if(!context)return FALSE;INT_PTR themed{};if(file_dialog_visual(dialog,context->appearance,message,wparam,lparam,themed))return themed;
    if(message==WM_SETTINGCHANGE||(message&&message==platform::win32::DesktopAppearance::changed_message())){context->appearance.reload();apply_file_manager_visuals(dialog,context->appearance);return TRUE;}
    if(message==WM_COMMAND&&(LOWORD(wparam)==IDOK||LOWORD(wparam)==IDCANCEL)){EndDialog(dialog,LOWORD(wparam));return TRUE;}
    if(message==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(message==WM_SIZE){readonly_layout(dialog,*context);return TRUE;}
    if(message==WM_GETMINMAXINFO){auto* limits=reinterpret_cast<MINMAXINFO*>(lparam);limits->ptMinTrackSize={MulDiv(360,context->dpi,96),MulDiv(240,context->dpi,96)};return TRUE;}
    if(message==WM_DESTROY&&context->font){DeleteObject(context->font);context->font=nullptr;}return FALSE;
}
}
bool show_readonly(HWND owner,platform::win32::DesktopAppearance& appearance,const std::wstring& title,const std::wstring& body,bool confirmation){
    struct Template {DLGTEMPLATE dialog;WORD menu{},cls{},caption{};} form{};form.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|DS_MODALFRAME|DS_CENTER;form.dialog.cx=450;form.dialog.cy=290;
    ReadOnlyText context{appearance,title,body,confirmation};
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&form.dialog,owner,readonly_proc,reinterpret_cast<LPARAM>(&context))==IDOK;
}
std::wstring normalized_form_lines(std::wstring_view source){std::wstring result;result.reserve(source.size());for(std::size_t n=0;n<source.size();++n){if(source[n]==L'\r'){result+=L'\n';if(n+1<source.size()&&source[n+1]==L'\n')++n;}else result+=source[n];}return result;}
std::wstring native_form_lines(std::wstring_view source){const auto normalized=normalized_form_lines(source);std::wstring result;result.reserve(normalized.size());for(const auto c:normalized){if(c==L'\n')result+=L'\r';result+=c;}return result;}
namespace {
template<class T> struct Com {T* p{};~Com(){if(p)p->Release();}T* operator->()const{return p;}T** out(){return &p;}};
int window_dpi(HWND window){const auto dc=GetDC(window);const auto dpi=dc?GetDeviceCaps(dc,LOGPIXELSX):96;if(dc)ReleaseDC(window,dc);return dpi>0?dpi:96;}
struct FormContext {
    platform::win32::DesktopAppearance& appearance;
    std::wstring title,explanation;
    std::vector<FormField> fields;
    FormValidation validate;
    std::vector<HWND> controls;
    HFONT font{};
    bool accepted{};
    int content_width{},content_height{},scroll_x{},scroll_y{};
    ~FormContext(){if(font)DeleteObject(font);}
};
void form_scroll(HWND dialog,FormContext& context,int x,int y){
    RECT client{};GetClientRect(dialog,&client);x=std::clamp(x,0,std::max(0,context.content_width-static_cast<int>(client.right)));y=std::clamp(y,0,std::max(0,context.content_height-static_cast<int>(client.bottom)));
    const auto dx=context.scroll_x-x,dy=context.scroll_y-y;context.scroll_x=x;context.scroll_y=y;SetScrollPos(dialog,SB_HORZ,x,TRUE);SetScrollPos(dialog,SB_VERT,y,TRUE);if(dx||dy)ScrollWindowEx(dialog,dx,dy,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);
}
LRESULT CALLBACK field_focus(HWND child,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR key,DWORD_PTR){
    if(message==WM_SETFOCUS)PostMessageW(GetParent(child),WM_APP+11,0,reinterpret_cast<LPARAM>(child));
    if(message==WM_NCDESTROY)RemoveWindowSubclass(child,field_focus,key);return DefSubclassProc(child,message,wparam,lparam);
}
std::wstring text(HWND control){const auto length=GetWindowTextLengthW(control);if(length<0||length>32768)return {};std::wstring result(static_cast<std::size_t>(length)+1,L'\0');const auto read=GetWindowTextW(control,result.data(),length+1);result.resize(static_cast<std::size_t>(std::max(0,read)));return result;}
INT_PTR CALLBACK form_proc(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam){
    auto* context=reinterpret_cast<FormContext*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG){context=reinterpret_cast<FormContext*>(lparam);SetWindowLongPtrW(dialog,DWLP_USER,lparam);SetWindowTextW(dialog,context->title.c_str());const auto dpi=window_dpi(dialog);const auto scale=[dpi](int n){return MulDiv(n,dpi,96);};NONCLIENTMETRICSW metrics{sizeof(metrics)};if(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0))context->font=CreateFontIndirectW(&metrics.lfMessageFont);
        const auto control=[&](const wchar_t* cls,const std::wstring& caption,DWORD style,int x,int y,int width,int height,UINT id){auto child=CreateWindowExW(cls==std::wstring_view(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,caption.c_str(),WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(width),scale(height),dialog,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);if(child&&context->font)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(context->font),TRUE);if(child)SetWindowSubclass(child,field_focus,2,0);return child;};
        int y=12;control(L"STATIC",context->explanation,SS_LEFT|SS_NOPREFIX,12,y,472,48,0);y+=54;
        for(std::size_t n=0;n<context->fields.size();++n){const auto& field=context->fields[n];const auto id=static_cast<UINT>(500+n);HWND child{};
            if(field.kind==FormFieldKind::checkbox){child=control(L"BUTTON",field.label,WS_TABSTOP|BS_AUTOCHECKBOX,12,y,472,24,id);Button_SetCheck(child,field.checked?BST_CHECKED:BST_UNCHECKED);y+=30;}
            else{control(L"STATIC",field.label,SS_LEFT|SS_NOPREFIX,12,y,472,18,0);y+=20;const bool multi=field.kind==FormFieldKind::multiline;const auto height=multi?100:25;
                if(field.kind==FormFieldKind::choice){child=control(L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,12,y,472,180,id);int selected{};for(std::size_t k=0;k<field.choices.size();++k){SendMessageW(child,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(field.choices[k].second.c_str()));if(field.choices[k].first==field.selected)selected=static_cast<int>(k);}SendMessageW(child,CB_SETCURSEL,selected,0);}
                else{child=control(L"EDIT",multi?native_form_lines(field.value):field.value,WS_TABSTOP|(multi?ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL:ES_AUTOHSCROLL),12,y,472,height,id);SendMessageW(child,EM_SETLIMITTEXT,multi?16384:8192,0);}y+=height+10;
            }context->controls.push_back(child);
        }
        control(L"BUTTON",L"Continue",WS_TABSTOP|BS_DEFPUSHBUTTON,266,y,104,28,IDOK);control(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,380,y,104,28,IDCANCEL);y+=42;
        context->content_width=scale(496);context->content_height=scale(y);RECT desired{0,0,context->content_width,context->content_height};AdjustWindowRectEx(&desired,static_cast<DWORD>(GetWindowLongPtrW(dialog,GWL_STYLE)),FALSE,0);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(GetWindow(dialog,GW_OWNER),MONITOR_DEFAULTTONEAREST),&monitor);const auto width=std::min(static_cast<int>(desired.right-desired.left),static_cast<int>(monitor.rcWork.right-monitor.rcWork.left)),height=std::min(static_cast<int>(desired.bottom-desired.top),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));SetWindowPos(dialog,nullptr,monitor.rcWork.left+std::max(0,static_cast<int>((monitor.rcWork.right-monitor.rcWork.left-width)/2)),monitor.rcWork.top+std::max(0,static_cast<int>((monitor.rcWork.bottom-monitor.rcWork.top-height)/2)),width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        for(unsigned pass=0;pass<2;++pass){RECT client{};GetClientRect(dialog,&client);SCROLLINFO vertical{sizeof(vertical),SIF_RANGE|SIF_PAGE|SIF_POS,0,context->content_height-1,static_cast<UINT>(std::max<LONG>(1,client.bottom)),0,0};SCROLLINFO horizontal{sizeof(horizontal),SIF_RANGE|SIF_PAGE|SIF_POS,0,context->content_width-1,static_cast<UINT>(std::max<LONG>(1,client.right)),0,0};SetScrollInfo(dialog,SB_VERT,&vertical,TRUE);SetScrollInfo(dialog,SB_HORZ,&horizontal,TRUE);ShowScrollBar(dialog,SB_VERT,context->content_height>client.bottom);ShowScrollBar(dialog,SB_HORZ,context->content_width>client.right);}
        apply_file_manager_visuals(dialog,context->appearance);return TRUE;
    }
    if(!context)return FALSE;INT_PTR themed{};if(file_dialog_visual(dialog,context->appearance,message,wparam,lparam,themed))return themed;
    if(message==WM_SETTINGCHANGE||(message&&message==platform::win32::DesktopAppearance::changed_message())){context->appearance.reload();apply_file_manager_visuals(dialog,context->appearance);return TRUE;}
    if(message==WM_VSCROLL||message==WM_HSCROLL){const bool vertical=message==WM_VSCROLL;const auto bar=vertical?SB_VERT:SB_HORZ;SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(dialog,bar,&info);int next=info.nPos;switch(LOWORD(wparam)){case SB_LINEUP:next-=24;break;case SB_LINEDOWN:next+=24;break;case SB_PAGEUP:next-=static_cast<int>(info.nPage);break;case SB_PAGEDOWN:next+=static_cast<int>(info.nPage);break;case SB_THUMBTRACK:case SB_THUMBPOSITION:next=info.nTrackPos;break;case SB_TOP:next=0;break;case SB_BOTTOM:next=info.nMax;break;default:return TRUE;}form_scroll(dialog,*context,vertical?context->scroll_x:next,vertical?next:context->scroll_y);return TRUE;}
    if(message==WM_MOUSEWHEEL){form_scroll(dialog,*context,context->scroll_x,context->scroll_y-GET_WHEEL_DELTA_WPARAM(wparam)/WHEEL_DELTA*72);return TRUE;}
    if(message==WM_APP+11){const auto child=reinterpret_cast<HWND>(lparam);if(IsChild(dialog,child)){RECT field{},client{};GetWindowRect(child,&field);MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&field),2);GetClientRect(dialog,&client);const auto x=context->scroll_x+(field.left<0?field.left:field.right>client.right?field.right-client.right:0),y=context->scroll_y+(field.top<0?field.top:field.bottom>client.bottom?field.bottom-client.bottom:0);form_scroll(dialog,*context,x,y);}return TRUE;}
    if(message==WM_COMMAND){if(LOWORD(wparam)==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}if(LOWORD(wparam)==IDOK){auto proposed=context->fields;
            for(std::size_t n=0;n<proposed.size();++n){auto& field=proposed[n];const auto child=context->controls[n];if(field.kind==FormFieldKind::checkbox)field.checked=Button_GetCheck(child)==BST_CHECKED;else if(field.kind==FormFieldKind::choice){const auto selected=SendMessageW(child,CB_GETCURSEL,0,0);if(selected>=0&&static_cast<std::size_t>(selected)<field.choices.size())field.selected=field.choices[static_cast<std::size_t>(selected)].first;}else{field.value=text(child);if(field.kind==FormFieldKind::multiline)field.value=normalized_form_lines(field.value);}}
            if(context->validate)if(const auto error=context->validate(proposed)){MessageBoxW(dialog,error->c_str(),L"Review the values",MB_OK|MB_ICONWARNING);return TRUE;}context->fields=std::move(proposed);context->accepted=true;EndDialog(dialog,IDOK);return TRUE;}}
    if(message==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}return FALSE;
}
}
PickerResult pick_import_sources(HWND owner,const std::filesystem::path& initial,bool folder){
    PickerResult result;Com<IFileOpenDialog> dialog;if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.out())))){result.error=L"Windows file picker is unavailable.";return result;}
    DWORD options{};if(FAILED(dialog->GetOptions(&options))||FAILED(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|(folder?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST|FOS_ALLOWMULTISELECT)))){result.error=L"Windows file picker could not be configured.";return result;}
    dialog->SetTitle(folder?L"Choose an import folder (not recursive)":L"Choose files to import");
    if(!initial.empty()){Com<IShellItem> start;if(SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(),nullptr,IID_PPV_ARGS(start.out()))))dialog->SetDefaultFolder(start.p);}
    const auto shown=dialog->Show(owner);if(shown==HRESULT_FROM_WIN32(ERROR_CANCELLED)){result.cancelled=true;return result;}if(FAILED(shown)){result.error=L"Windows file selection failed. Nothing imported.";return result;}
    Com<IShellItemArray> items;if(FAILED(dialog->GetResults(items.out()))){result.error=L"Selected files are unavailable.";return result;}DWORD count{};if(FAILED(items->GetCount(&count))||!count||count>1000){result.error=L"Choose between one and 1,000 items per batch. Nothing imported.";return result;}
    for(DWORD n=0;n<count;++n){Com<IShellItem> item;PWSTR path{};if(FAILED(items->GetItemAt(n,item.out()))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))||!path){result.paths.clear();result.error=L"A selection is not a filesystem item. Nothing imported.";return result;}result.paths.emplace_back(path);CoTaskMemFree(path);}return result;
}
std::optional<std::vector<FormField>> show_form(HWND owner,platform::win32::DesktopAppearance& appearance,std::wstring title,std::wstring explanation,std::vector<FormField> fields,FormValidation validate){
    if(fields.size()>6)return {};FormContext context{appearance,std::move(title),std::move(explanation),std::move(fields),std::move(validate)};
    // Empty native modal template; controls are created with current system
    // message font and operator appearance, not fixed 8-bit dialog text.
    struct alignas(DWORD) Template {DLGTEMPLATE value;WORD menu{},cls{},caption{};} data{};data.value.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;data.value.dwExtendedStyle=WS_EX_CONTROLPARENT;data.value.cx=300;data.value.cy=240;
    const auto outcome=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&data.value,owner,form_proc,reinterpret_cast<LPARAM>(&context));return outcome==IDOK&&context.accepted?std::optional<std::vector<FormField>>{std::move(context.fields)}:std::nullopt;
}
} // namespace sfx::file_manager
