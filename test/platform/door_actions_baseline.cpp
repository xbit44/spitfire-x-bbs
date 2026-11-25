// Reproduce the owner-visible mismatch before any product correction.
#define main retained_actions_early_main
#define render retained_actions_early_render
#include "door_command_line_early.cpp"
#undef render
#undef main
namespace {
LRESULT CALLBACK actions_root(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
 auto& s=*reinterpret_cast<State*>(data);if(m==WM_NCDESTROY)RemoveWindowSubclass(h,actions_root,id);
 if(m==WM_NOTIFY){const auto* n=reinterpret_cast<NMHDR*>(l);if(const auto v=door_navigation_notify(s,n))return *v;
 if(n&&n->hwndFrom==s.navigation&&!s.navigating){if(n->code==TVN_SELCHANGINGW)return resolve_dirty(s)?FALSE:TRUE;if(n->code==TVN_SELCHANGEDW){door_navigation_open(s,reinterpret_cast<NMTREEVIEWW*>(l)->itemNew.hItem);return 0;}}
 if(const auto v=door_configuration_notify(s,n))return *v;}
 if(m==WM_COMMAND&&door_configuration_command(s,LOWORD(w),HIWORD(w)))return 0;return DefSubclassProc(h,m,w,l);
}
}
int main(int,char**){try{
 State s;s.dpi=96;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\actions-baseline-not-installed";INITCOMMONCONTROLSEX c{sizeof(c),ICC_WIN95_CLASSES};InitCommonControlsEx(&c);
 s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owned actions baseline",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1050,820,nullptr,nullptr,s.instance,nullptr);check(s.window&&create_ui(s)&&SetWindowSubclass(s.window,actions_root,96,reinterpret_cast<DWORD_PTR>(&s)),"native controls");door_catalog_fixture=snapshot;door_change_fixture=change;seed();catalog.menus[1].name="Sci-Fi";for(auto& e:catalog.entries)if(e.kind==app::DoorEntryKind::submenu&&e.target==2)e.title="Sci-Fi";catalog.entries.erase(std::remove_if(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return e.menu==2;}),catalog.entries.end());refresh_door_configuration_page(s);show_page(s,26);remember_page(s);
 HTREEITEM sci{};for(const auto& n:s.doors.navigation_items)if(n.entry.kind==app::DoorEntryKind::submenu&&n.entry.target==2)sci=n.handle;check(sci!=nullptr,"left Sci-Fi");TreeView_SelectItem(s.navigation,sci);check(s.doors.menu==2&&s.doors.view==DoorPage::entries,"Sci-Fi open");press(s,IDC_DOOR_EDIT);const bool edit=s.doors.view==DoorPage::entries;press(s,IDC_DOOR_PLACEMENT_EDIT);const bool placement=s.doors.view==DoorPage::entries;press(s,IDC_DOOR_SHOW_ANOTHER);const bool show=s.doors.view==DoorPage::entries;press(s,IDC_DOOR_CATEGORY_EDIT);const bool wrong=s.doors.category_id!=2;DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);door_catalog_fixture=nullptr;door_change_fixture=nullptr;
 std::cout<<"REPRODUCED edit_noop="<<edit<<" placement_noop="<<placement<<" show_noop="<<show<<" category_wrong_target="<<wrong<<'\n';return edit&&placement&&show&&wrong?2:3;
}catch(const std::exception& e){std::cerr<<"FAIL baseline: "<<e.what()<<'\n';return 1;}}
