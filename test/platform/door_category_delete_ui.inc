template<class Capture>void category_delete_ui_checks(State& s,const Capture& capture){
    auto& d=s.doors;std::vector<std::uint8_t> reference;
    aseed(s,false);aselect(s,true);const auto before_keys=acatalog();s.current_page=0;
    SendMessageW(s.navigation,WM_KEYDOWN,VK_DELETE,0);check(d.view==DoorPage::entries&&acatalog()==before_keys,"Delete outside Doors ignores stale Door selection");s.current_page=26;
    select_list(d.entries,9);door_configuration_command(s,IDC_DOOR_ENTRIES,LBN_SELCHANGE);
    SendMessageW(s.navigation,WM_KEYDOWN,VK_DELETE,0);check(d.view==DoorPage::confirm&&d.confirmation->menu.id==2,"sidebar Delete uses actual sidebar category rather than selected right child");press(s,IDC_DOOR_CANCEL);
    for(const bool left:{true,false}){
        aseed(s,false);catalog.entries.push_back({8,4,app::DoorEntryKind::door,17,"Shared Door",""});catalog.entries.push_back({8,5,app::DoorEntryKind::submenu,9,"Shared child",""});++catalog.revision;refresh_door_configuration_page(s);remember_page(s);aselect(s,left);
        check(text(abutton(s,IDC_DOOR_DELETE_SELECTED))==L"Delete Category"&&IsWindowEnabled(abutton(s,IDC_DOOR_DELETE_SELECTED)),"both surfaces offer Delete Category for populated category");
        const auto before=acatalog();const auto count=changes;press(s,IDC_DOOR_DELETE_SELECTED);
        check(d.view==DoorPage::confirm&&d.confirmation&&d.confirmation->action==app::DoorConfigurationAction::remove_menu&&d.confirmation_steps.empty(),"single warning and single atomic category request");
        check(text(d.confirm_text).find(L"1 subcategories and 1 configured Doors")!=std::wstring::npos&&text(d.confirm_text).find(L"ALL menus")!=std::wstring::npos&&text(d.confirm_text).find(L"Game files stay on disk")!=std::wstring::npos,"warning counts recursive scope and explains global references and file boundary");
        check(GetFocus()==d.confirm_cancel,"destructive confirmation defaults focus to Cancel");capture(left?"11-left-delete-warning":"12-right-delete-warning");
        press(s,IDC_DOOR_CANCEL);check(acatalog()==before&&changes==count,"Cancel deletes nothing");if(!left)aselect(s,false);
        SendMessageW(left?s.navigation:d.categories,WM_KEYDOWN,VK_DELETE,0);check(d.view==DoorPage::confirm,"Delete key selects same category deletion on either surface");
        if(left)lose_at=changes+1;press(s,IDC_DOOR_CONFIRM);
        if(left){check(d.pending&&changes==count+1,"lost atomic deletion reply retained");press(s,IDC_DOOR_RECONCILE);}
        check(!d.pending&&changes==count+1&&d.view==DoorPage::categories&&d.menu==0,"one deletion returns to root without second warning or extra writes");
        check(!app::find_door_menu(catalog,2)&&!app::find_door_menu(catalog,9)&&!app::find_door(catalog,17)&&app::find_door(catalog,43)&&app::find_door_menu(catalog,8),"contained shared definitions deleted globally; unrelated items survive");
        check(std::none_of(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return (e.kind==app::DoorEntryKind::door&&e.target==17)||(e.kind==app::DoorEntryKind::submenu&&(e.target==2||e.target==9));}),"no leftover appearances of deleted configs");
        if(left)reference=acatalog();else check(reference==acatalog(),"left/right category delete results byte-equivalent");
    }
    aseed(s);TreeView_SelectItem(d.categories,anode(s,d.categories,app::DoorEntryKind::submenu,9));door_update_delete_actions(s);check(IsWindowEnabled(abutton(s,IDC_DOOR_DELETE_SELECTED)),"unplaced category deletable");press(s,IDC_DOOR_DELETE_SELECTED);press(s,IDC_DOOR_CONFIRM);check(!app::find_door_menu(catalog,9),"unplaced category really removed");
    aseed(s,false);aselect(s,true);press(s,IDC_DOOR_DELETE_SELECTED);++catalog.revision;const auto concurrent=acatalog();press(s,IDC_DOOR_CONFIRM);check(d.conflict&&acatalog()==concurrent,"server revision change refuses stale warning atomically");press(s,IDC_DOOR_RELOAD_DRAFT);
    aseed(s,false);aselect(s,true);press(s,IDC_DOOR_DELETE_SELECTED);++catalog.revision;d.catalog=catalog;const auto refreshed=acatalog();press(s,IDC_DOOR_CONFIRM);check(acatalog()==refreshed&&!d.pending,"local refresh cannot rebase previously confirmed deletion");press(s,IDC_DOOR_CANCEL);
}
