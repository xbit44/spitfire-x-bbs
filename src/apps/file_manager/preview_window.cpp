#include "preview_window.hpp"
#include "visuals.hpp"
#include "sfx/application/control_panel.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
namespace sfx::file_manager {
namespace app=application;namespace win=platform::win32;
namespace {
constexpr wchar_t view_class[]=L"SFXFileManagerPreviewWindow";
enum {view_previous=1301,view_next,view_archive,view_member,view_entries};
std::wstring wide(std::string_view value){const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(n<=0)return {};std::wstring out(static_cast<std::size_t>(n),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n);return out;}
std::uint32_t ega(unsigned color){const auto& p=app::kControlPanelMoebiusEgaPalette[color&15U];const auto channel=[](unsigned c){return (c<<2)|(c>>4);};return (channel(p[0])<<16)|(channel(p[1])<<8)|channel(p[2]);}
}
struct PreviewWindow::Impl {
    HINSTANCE instance;HWND owner,window{},previous{},next{},archive{},member{},entries{},status{};
    win::DesktopAppearance& appearance;
    std::function<void(ViewNavigation)> notify;
    app::FileManagementViewPage page;
    std::array<std::uint8_t,4096> font{};
    HFONT ui_font{};
    bool loaded{},pending{};int content_top{44};
    Impl(HINSTANCE i,HWND o,win::DesktopAppearance& a,std::function<void(ViewNavigation)> n):instance(i),owner(o),appearance(a),notify(std::move(n)){
        const auto resource=FindResourceW(instance,MAKEINTRESOURCEW(201),RT_RCDATA);if(resource&&SizeofResource(instance,resource)==font.size()){const auto bytes=static_cast<const std::uint8_t*>(LockResource(LoadResource(instance,resource)));if(bytes){std::copy_n(bytes,font.size(),font.begin());loaded=true;}}
        NONCLIENTMETRICSW metrics{sizeof(metrics)};if(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0))ui_font=CreateFontIndirectW(&metrics.lfMessageFont);
    }
    ~Impl(){notify={};if(window)DestroyWindow(window);if(ui_font)DeleteObject(ui_font);}
    void layout(){RECT client{};GetClientRect(window,&client);const auto dc=GetDC(window);const auto dpi=dc?GetDeviceCaps(dc,LOGPIXELSX):96;if(dc)ReleaseDC(window,dc);const auto s=[dpi](int n){return MulDiv(n,dpi>0?dpi:96,96);};const bool wrap=client.right<s(540);content_top=s(wrap?78:44);MoveWindow(previous,s(8),s(8),s(96),s(28),TRUE);MoveWindow(next,s(112),s(8),s(96),s(28),TRUE);MoveWindow(archive,s(wrap?8:216),s(wrap?42:8),s(142),s(28),TRUE);MoveWindow(member,s(wrap?158:366),s(wrap?42:8),s(150),s(28),TRUE);MoveWindow(entries,s(8),content_top,std::max(20,static_cast<int>(client.right)-s(16)),std::max(20,static_cast<int>(client.bottom)-content_top-s(34)),TRUE);MoveWindow(status,s(8),std::max(content_top,static_cast<int>(client.bottom)-s(28)),std::max(20,static_cast<int>(client.right)-s(16)),s(22),TRUE);ShowWindow(entries,page.archive?SW_SHOW:SW_HIDE);ShowWindow(member,page.archive?SW_SHOW:SW_HIDE);ShowWindow(archive,!page.archive&&!page.member.empty()?SW_SHOW:SW_HIDE);}
    void enabled(){const auto count=page.archive?app::kFileViewArchivePage:app::kFileViewRows;EnableWindow(previous,!pending&&page.page>0);EnableWindow(next,!pending&&(static_cast<std::size_t>(page.page)+1)*count<page.total);EnableWindow(archive,!pending&&!page.archive&&!page.member.empty());EnableWindow(member,!pending&&page.archive&&ListView_GetNextItem(entries,-1,LVNI_SELECTED)>=0);}
    void navigation(ViewNavigationKind kind,std::uint16_t selection=0){if(!pending&&notify)notify({kind,page.cursor,selection});}
    void selected(){const auto row=ListView_GetNextItem(entries,-1,LVNI_SELECTED);if(row<0||static_cast<std::size_t>(row)>=page.entries.size())return;const auto& entry=page.entries[static_cast<std::size_t>(row)];if(entry.readable)navigation(ViewNavigationKind::member,entry.index);else SetWindowTextW(status,L"This entry is list-only (directory, encrypted or unsupported).");}
    void update(){SetWindowTextW(window,(L"File Preview — "+wide(page.filename)+(page.member.empty()?L"":L" / "+wide(page.member))).c_str());ListView_DeleteAllItems(entries);
        for(std::size_t n=0;n<page.entries.size();++n){const auto& e=page.entries[n];auto name=wide(e.name);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(n);item.pszText=name.data();const auto row=ListView_InsertItem(entries,&item);auto size=std::to_wstring(e.bytes);ListView_SetItemText(entries,row,1,size.data());std::wstring type=e.directory?L"Directory":e.encrypted?L"Encrypted / list-only":e.readable?L"May preview text/ANSI":L"List-only";ListView_SetItemText(entries,row,2,type.data());}
        const auto label=L"Read-only — Page "+std::to_wstring(static_cast<unsigned>(page.page)+1)+L" / "+std::to_wstring(std::max<std::size_t>(1,(page.total+(page.archive?19:24))/(page.archive?20:25)))+L". Nothing extracted or executed.";SetWindowTextW(status,label.c_str());layout();enabled();InvalidateRect(window,nullptr,TRUE);}
    void paint(HDC dc){RECT client{};GetClientRect(window,&client);FillRect(dc,&client,appearance.background_brush());if(page.archive||!loaded)return;const auto status_top=[&]{RECT r{};GetWindowRect(status,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);return r.top;}();const int available_w=std::max(1,static_cast<int>(client.right)-16),available_h=std::max(1,static_cast<int>(status_top)-content_top-8);const int width=std::min(available_w,available_h*640/400),height=width*400/640;const int origin_x=(static_cast<int>(client.right)-width)/2,origin_y=content_top;
        std::vector<std::uint32_t> pixels(640U*400U,0);for(std::size_t n=0;n<page.cells.size();++n){const auto& cell=page.cells[n];const auto fg=ega(cell.attribute),bg=ega((cell.attribute>>4)&7);const auto left=(n%80)*8,up=(n/80)*16;for(unsigned y=0;y<16;++y){const auto bits=font[static_cast<std::size_t>(cell.glyph)*16+y];for(unsigned x=0;x<8;++x)pixels[(up+y)*640+left+x]=(bits&(0x80U>>x))?fg:bg;}}
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=640;info.bmiHeader.biHeight=-400;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;const auto old=SetStretchBltMode(dc,COLORONCOLOR);StretchDIBits(dc,origin_x,origin_y,width,height,0,0,640,400,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);if(old)SetStretchBltMode(dc,old);
    }
    static LRESULT CALLBACK proc(HWND handle,UINT message,WPARAM wparam,LPARAM lparam){auto* p=reinterpret_cast<Impl*>(GetWindowLongPtrW(handle,GWLP_USERDATA));if(message==WM_NCCREATE){p=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);p->window=handle;SetWindowLongPtrW(handle,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}if(!p)return DefWindowProcW(handle,message,wparam,lparam);LRESULT themed{};if(p->appearance.control_message(handle,message,wparam,lparam,themed))return themed;
        switch(message){
        case WM_CREATE:{const auto child=[&](const wchar_t* cls,const wchar_t* caption,DWORD style,UINT id){const auto control=CreateWindowExW(0,cls,caption,WS_CHILD|WS_VISIBLE|(wcscmp(cls,L"STATIC")?WS_TABSTOP:0)|style,0,0,20,20,handle,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),p->instance,nullptr);if(p->ui_font)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(p->ui_font),TRUE);return control;};p->previous=child(L"BUTTON",L"Previous",BS_PUSHBUTTON,view_previous);p->next=child(L"BUTTON",L"Next",BS_PUSHBUTTON,view_next);p->archive=child(L"BUTTON",L"Archive index",BS_PUSHBUTTON,view_archive);p->member=child(L"BUTTON",L"Preview selected",BS_PUSHBUTTON,view_member);p->entries=child(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER,view_entries);ListView_SetExtendedListViewStyle(p->entries,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);for(unsigned n=0;n<3;++n){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.cx=n==0?400:n==1?120:200;column.pszText=const_cast<wchar_t*>(n==0?L"Member":n==1?L"Bytes":L"Preview");ListView_InsertColumn(p->entries,static_cast<int>(n),&column);}p->status=child(L"STATIC",L"Read-only",SS_LEFT|SS_NOPREFIX,0);apply_file_manager_visuals(handle,p->appearance);return 0;}
        case WM_GETMINMAXINFO:{const auto dc=GetDC(handle);const auto dpi=dc?GetDeviceCaps(dc,LOGPIXELSX):96;if(dc)ReleaseDC(handle,dc);auto* limits=reinterpret_cast<MINMAXINFO*>(lparam);limits->ptMinTrackSize={MulDiv(340,dpi>0?dpi:96,96),MulDiv(240,dpi>0?dpi:96,96)};return 0;}
        case WM_SIZE:p->layout();InvalidateRect(handle,nullptr,TRUE);return 0;
        case WM_PAINT:{PAINTSTRUCT paint_info{};const auto dc=BeginPaint(handle,&paint_info);p->paint(dc);EndPaint(handle,&paint_info);return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_COMMAND:switch(LOWORD(wparam)){case view_previous:if(p->page.page)p->navigation(ViewNavigationKind::page,static_cast<std::uint16_t>(p->page.page-1));break;case view_next:p->navigation(ViewNavigationKind::page,static_cast<std::uint16_t>(p->page.page+1));break;case view_archive:p->navigation(ViewNavigationKind::archive_index);break;case view_member:case IDOK:p->selected();break;case IDCANCEL:SendMessageW(handle,WM_CLOSE,0,0);break;default:break;}return 0;
        case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lparam)->hwndFrom==p->entries){p->enabled();if(reinterpret_cast<NMHDR*>(lparam)->code==NM_DBLCLK)p->selected();}return 0;
        case WM_CLOSE:if(p->notify)p->notify({ViewNavigationKind::close,p->page.cursor,0});DestroyWindow(handle);return 0;
        case WM_NCDESTROY:p->window=nullptr;SetWindowLongPtrW(handle,GWLP_USERDATA,0);break;
        default:if((message==win::DesktopAppearance::changed_message()&&message!=0)||message==WM_SETTINGCHANGE){p->appearance.reload();apply_file_manager_visuals(handle,p->appearance);InvalidateRect(handle,nullptr,TRUE);}break;
        }return DefWindowProcW(handle,message,wparam,lparam);
    }
};
PreviewWindow::PreviewWindow(HINSTANCE instance,HWND owner,win::DesktopAppearance& appearance,std::function<void(ViewNavigation)> notify):impl_(std::make_unique<Impl>(instance,owner,appearance,std::move(notify))){}
PreviewWindow::~PreviewWindow()=default;
bool PreviewWindow::show(const app::FileManagementViewPage& page){auto& p=*impl_;if(!p.loaded||!app::valid_file_management_view(page))return false;p.page=page;p.pending=false;if(!p.window){WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=Impl::proc;cls.hInstance=p.instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=view_class;cls.hIcon=LoadIconW(p.instance,MAKEINTRESOURCEW(1));if(!GetClassInfoExW(p.instance,view_class,&cls)){cls=WNDCLASSEXW{sizeof(cls)};cls.lpfnWndProc=Impl::proc;cls.hInstance=p.instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=view_class;cls.hIcon=LoadIconW(p.instance,MAKEINTRESOURCEW(1));if(!RegisterClassExW(&cls))return false;}if(!CreateWindowExW(WS_EX_CONTROLPARENT,view_class,L"File Preview",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,860,650,p.owner,nullptr,p.instance,&p))return false;}p.update();ShowWindow(p.window,SW_SHOWNORMAL);SetForegroundWindow(p.window);return true;}
void PreviewWindow::busy(bool value){impl_->pending=value;if(impl_->window)impl_->enabled();}
void PreviewWindow::close(){if(impl_->window)DestroyWindow(impl_->window);}
bool PreviewWindow::translate(MSG& message){auto& p=*impl_;if(!p.window)return false;if(message.message==WM_KEYDOWN&&(message.hwnd==p.window||IsChild(p.window,message.hwnd))){const auto count=p.page.archive?app::kFileViewArchivePage:app::kFileViewRows;if(message.wParam==VK_PRIOR){if(p.page.page)p.navigation(ViewNavigationKind::page,static_cast<std::uint16_t>(p.page.page-1));return true;}if(message.wParam==VK_NEXT){if((static_cast<std::size_t>(p.page.page)+1)*count<p.page.total)p.navigation(ViewNavigationKind::page,static_cast<std::uint16_t>(p.page.page+1));return true;}}return IsDialogMessageW(p.window,&message)!=FALSE;}
HWND PreviewWindow::window()const noexcept{return impl_->window;}
} // namespace sfx::file_manager
