// Both browser surfaces resolve the same current item. A category's own
// placement can live in a different menu from the contents being displayed.
std::optional<DoorEditorState::TreeItem> door_action_item(State& s){
    auto& d=s.doors;if(!d.catalog)return {};
    std::optional<DoorEditorState::TreeItem> item;
    if(d.view==DoorPage::categories)item=door_tree_item(s);
    else if(d.view==DoorPage::entries){
        const auto row=SendMessageW(d.entries,LB_GETCURSEL,0,0);
        if(row!=LB_ERR){const auto number=SendMessageW(d.entries,LB_GETITEMDATA,row,0);for(const auto& e:d.catalog->entries)if(e.menu==d.menu&&e.number==number){item=DoorEditorState::TreeItem{nullptr,e};break;}}
        else if(d.navigation_placement&&d.navigation_placement->target==d.menu)item=DoorEditorState::TreeItem{nullptr,*d.navigation_placement,d.navigation_placement->number==0&&d.menu!=0};
        else if(const auto* menu=app::find_door_menu(*d.catalog,d.menu))item=DoorEditorState::TreeItem{nullptr,{0,0,app::DoorEntryKind::submenu,d.menu,menu->name,{}},d.menu!=0};
    }
    if(!item||item->group)return {};
    if(item->entry.number){for(const auto& e:d.catalog->entries)if(e.menu==item->entry.menu&&e.number==item->entry.number&&e.kind==item->entry.kind&&e.target==item->entry.target){item->entry=e;return item;}return {};}
    if(item->entry.kind==app::DoorEntryKind::submenu?app::find_door_menu(*d.catalog,item->entry.target)!=nullptr:app::find_door(*d.catalog,item->entry.target)!=nullptr)return item;
    return {};
}
void door_action_add_context(State& s){
    auto& d=s.doors;if(const auto item=door_action_item(s)){
        if(item->entry.kind==app::DoorEntryKind::submenu){d.menu=item->entry.target;d.entry=0;d.navigation_placement=item->entry;}
        else d.menu=item->entry.menu;
    }
}
void door_update_delete_actions(State& s){
    auto& d=s.doors;const auto item=door_action_item(s);const bool ready=d.catalog&&!d.pending&&!d.conflict;
    const bool category=item&&item->entry.kind==app::DoorEntryKind::submenu,door=item&&!category,placed=item&&item->entry.number;
    std::wstring target=L"Select a category or Door.";if(item)target=(category?L"Selected category: ":L"Selected Door: ")+utf8_to_wide(item->entry.title);
    for(const auto h:d.action_detail)if(h)SetWindowTextW(h,target.c_str());
    for(const auto view:{DoorPage::categories,DoorPage::entries})for(const auto h:d.controls[static_cast<unsigned>(view)]){
        const auto id=GetDlgCtrlID(h);bool allowed=true;
        if(id==IDC_DOOR_EDIT||id==IDC_DOOR_CATEGORY_EDIT){SetWindowTextW(h,category?L"Edit Name / Security Levels":L"Edit Door");allowed=bool(item);}
        else if(id==IDC_DOOR_OPEN){SetWindowTextW(h,L"Open Category");allowed=category&&(view!=DoorPage::entries||item->entry.target!=d.menu);}
        else if(id==IDC_DOOR_PLACEMENT_EDIT||id==IDC_DOOR_SHOW_ANOTHER||id==IDC_DOOR_ENTRY_REMOVE)allowed=placed;
        else if(id==IDC_DOOR_DELETE_SELECTED){SetWindowTextW(h,category?L"Delete Category":L"Delete Door");allowed=door||(category&&item->entry.target);}
        else if(id==IDC_DOOR_MENU_DUPLICATE)allowed=category&&app::next_unused_door_menu_id(*d.catalog).has_value();
        else if(id==IDC_DOOR_NEW||id==IDC_DOOR_TREE_ADD_DOOR||id==IDC_DOOR_MENU_NEW||id==IDC_DOOR_ADD_EXISTING){
            const auto parent=category?item->entry.target:door?item->entry.menu:d.menu;allowed=item&&door_free_number(*d.catalog,parent)!=0;
            if(id==IDC_DOOR_MENU_NEW)allowed=allowed&&app::next_unused_door_menu_id(*d.catalog).has_value();
            else if(id==IDC_DOOR_ADD_EXISTING)allowed=allowed&&!d.catalog->doors.empty();
            else allowed=allowed&&app::next_unused_door_id(*d.catalog).has_value();
        }else continue;
        EnableWindow(h,ready&&allowed);
    }
}
