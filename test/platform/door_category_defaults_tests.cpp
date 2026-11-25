// Actual SFXConfig controls and existing synthetic catalog/save port. No images,
// installation, real accounts or settings are created by this focused probe.
#define main retained_category_defaults_main
#include "door_command_line_early.cpp"
#undef main

int main(){try{
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};
    check(InitCommonControlsEx(&common)!=FALSE,"native classes");
    for(const int dpi:{96,144,192}){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);
        s.root=L"C:\\sfxtst\\category-defaults-synthetic-only";
        s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owned category-default controls",
            WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1080,860,nullptr,nullptr,s.instance,nullptr);
        check(s.window&&create_ui(s),"create hidden actual configuration controls");
        door_catalog_fixture=snapshot;door_change_fixture=change;
        show_page(s,26);auto& d=s.doors;
        for(const app::DoorMenuId parent:{app::DoorMenuId{0},app::DoorMenuId{2}}){
            seed();d.menu=parent;d.entry=0;d.navigation_placement.reset();
            d.view=parent?DoorPage::entries:DoorPage::categories;
            refresh_door_configuration_page(s);remember_page(s);
            if(parent)SendMessageW(d.entries,LB_SETCURSEL,static_cast<WPARAM>(-1),0);
            const auto before=app::encode_door_catalog(catalog);
            press(s,IDC_DOOR_MENU_NEW);
            check(d.category_new&&d.view==DoorPage::category,"Add Category enters new category form");
            check(door_text(d.category_requirements[1])=="10"&&door_text(d.category_requirements[4])=="10",
                "root and nested Category controls default to access10/execution10");
            check(SendMessageW(d.category_requirements[0],CB_GETCURSEL,0,0)==0&&
                  SendMessageW(d.category_requirements[3],CB_GETCURSEL,0,0)==0&&
                  !door_checked(d.category_requirements[2])&&!door_checked(d.category_requirements[5]),
                  "new requirements stay minimum levels with Sysop Only unchecked");
            check(door_selected(d.category_parent,true)==parent,"new category retains intended parent");
            press(s,IDC_DOOR_CANCEL);
            check(app::encode_door_catalog(catalog)==before,"cancel creates no category or changed settings");
            press(s,IDC_DOOR_MENU_NEW);const auto id=d.category_id;
            door_set(d.category_name,parent?"New nested category":"New root category");
            check(save_door_configuration_page(s),"save new category using existing protocol/model port");
            const auto* saved=app::find_door_menu(catalog,id);
            check(saved&&saved->access.level==10&&saved->execution.level==10,"both defaults survive saved catalog");
            check(!app::meets_door_requirements(saved->access,{9,999})&&
                   app::meets_door_requirements(saved->access,{10,999})&&
                  !app::meets_door_requirements(saved->execution,{9,999})&&
                   app::meets_door_requirements(saved->execution,{10,999}),"security boundary is9 denied10 allowed");
            const auto bytes=app::encode_door_catalog(catalog);
            const auto decoded=app::decode_door_catalog(bytes);
            check(decoded&&app::encode_door_catalog(*decoded)==bytes,"10/10 persists through existing record codec");
            for(const auto& legacy:catalog.menus)if(legacy.id!=id)
                check(legacy.access.level==1&&legacy.execution.level==1,"preexisting root/categories remain1/1");
        }
        for(const bool customized:{false,true}){
            seed();if(customized){catalog.menus[1].access={app::DoorSecurityMode::exact,23,true};
                                catalog.menus[1].execution={app::DoorSecurityMode::maximum,47,false};}
            d.category_id=2;d.category_new=false;d.category_place_new=false;d.return_view=DoorPage::categories;
            d.menu=0;d.entry=0;d.navigation_placement.reset();
            refresh_door_configuration_page(s);door_load_category(s);door_show_view(s,DoorPage::category);remember_page(s);
            const auto expected_access=customized?23:1,expected_execution=customized?47:1;
            check(door_text(d.category_requirements[1])==std::to_string(expected_access)&&
                  door_text(d.category_requirements[4])==std::to_string(expected_execution),"edit retains legacy and custom levels");
            const auto before=app::encode_door_catalog(catalog);
            press(s,IDC_DOOR_CANCEL);check(app::encode_door_catalog(catalog)==before,"editing then cancel preserves saved bytes");
            d.category_id=2;d.category_new=false;d.category_place_new=false;
            door_load_category(s);door_show_view(s,DoorPage::category);remember_page(s);
            door_set(d.category_name,"Renamed existing category");
            check(save_door_configuration_page(s),"rename existing category");
            const auto* saved=app::find_door_menu(catalog,2);
            check(saved&&saved->access.level==expected_access&&saved->execution.level==expected_execution,
                  "rename does not reset existing security requirements");
            check(saved->access.mode==(customized?app::DoorSecurityMode::exact:app::DoorSecurityMode::minimum)&&
                  saved->execution.mode==(customized?app::DoorSecurityMode::maximum:app::DoorSecurityMode::minimum)&&
                  saved->access.sysop_only==customized&&!saved->execution.sysop_only,"existing comparison modes and Sysop flags retained");
        }
        DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);
        door_catalog_fixture=nullptr;door_change_fixture=nullptr;
    }
    std::cout<<"PASS "<<checks<<" category defaults checks; root/nested creation, save/cancel, boundary9/10, codec and existing settings; DPI96/144/192; source-only candidate, r6 unchanged\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
