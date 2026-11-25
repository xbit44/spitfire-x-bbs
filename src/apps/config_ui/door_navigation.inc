// Persistent global navigation. This tree owns only bounded placement metadata;
// the existing right-side editor owns drafts, saving and conflict recovery.
std::optional<DoorEditorState::TreeItem> door_navigation_item(State& s,HTREEITEM handle){
    if(handle==s.route_items[26])return DoorEditorState::TreeItem{handle,{0,0,app::DoorEntryKind::submenu,0,"Door Menu",{}}};
    for(const auto& item:s.doors.navigation_items)if(item.handle==handle)return item;return {};
}
void door_navigation_expand(State& s,HTREEITEM parent){
    auto& d=s.doors;if(!d.catalog||TreeView_GetChild(s.navigation,parent))return;
    const auto node=door_navigation_item(s,parent);if(!node||node->entry.kind!=app::DoorEntryKind::submenu)return;
    unsigned depth{};for(auto h=parent;h&&h!=s.route_items[26];h=TreeView_GetParent(s.navigation,h))++depth;
    if(depth>2 || d.navigation_items.size()+99>20000){set_status(s,L"Collapse other Door branches before expanding more categories.");return;}
    for(const auto& e:door_tree_entries(*d.catalog,node->entry.target)){
        std::wstring label=std::to_wstring(e.number)+L"  "+utf8_to_wide(e.title);
        if(e.kind==app::DoorEntryKind::door)if(const auto* door=app::find_door(*d.catalog,e.target)){if(!app::door_has_command(*door))label+=L" [Unfinished]";else if(!door->enabled)label+=L" [Disabled]";}
        TVINSERTSTRUCTW insert{};insert.hParent=parent;insert.hInsertAfter=TVI_LAST;
        insert.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_CHILDREN;insert.item.pszText=label.data();insert.item.lParam=26;
        insert.item.cChildren=e.kind==app::DoorEntryKind::submenu&&!door_tree_entries(*d.catalog,e.target).empty();
        const auto handle=TreeView_InsertItem(s.navigation,&insert);d.navigation_items.push_back({handle,e});
    }
}
void door_navigation_refresh(State& s){
    auto& d=s.doors;const auto root=s.route_items[26];if(!root||!d.catalog||d.navigation_revision==d.catalog->revision)return;
    const auto guard=s.navigating;s.navigating=true;
    while(const auto child=TreeView_GetChild(s.navigation,root))TreeView_DeleteItem(s.navigation,child);
    d.navigation_items.clear();d.navigation_revision=d.catalog->revision;
    TVITEMW item{};item.mask=TVIF_TEXT|TVIF_CHILDREN;item.hItem=root;
    item.pszText=const_cast<wchar_t*>(L"Door Menu");item.cChildren=!door_tree_entries(*d.catalog,0).empty();TreeView_SetItem(s.navigation,&item);
    door_navigation_expand(s,root);TreeView_Expand(s.navigation,root,TVE_EXPAND);s.navigating=guard;
}
bool door_navigation_path(const app::DoorCatalog& catalog,app::DoorMenuId from,app::DoorMenuId target,
    unsigned depth,std::vector<app::DoorMenuEntry>& path){
    if(from==target)return true;if(depth>=2)return false;
    for(const auto& e:door_tree_entries(catalog,from))if(e.kind==app::DoorEntryKind::submenu){
        path.push_back(e);if(door_navigation_path(catalog,e.target,target,depth+1,path))return true;path.pop_back();
    }return false;
}
void door_navigation_sync(State& s){
    auto& d=s.doors;if(!d.catalog||!s.route_items[26]||d.navigation_loading||s.current_page!=26)return;
    const auto guard=s.navigating;s.navigating=true;auto handle=s.route_items[26];
    std::vector<app::DoorMenuEntry> path;
    const bool placement=d.navigation_placement&&d.navigation_placement->number&&d.navigation_placement->target==d.menu;
    const auto parent=placement?d.navigation_placement->menu:d.menu;
    if(d.view!=DoorPage::categories && door_navigation_path(*d.catalog,0,parent,0,path)){
        if(placement)path.push_back(*d.navigation_placement);
        if(d.editing()&&d.edit_placement){if(const auto* e=door_current_entry(d))path.push_back(*e);}
        else if(d.view==DoorPage::entries){if(const auto selected=door_action_item(s);selected&&selected->entry.number&&selected->entry.menu==d.menu)path.push_back(selected->entry);}
        for(const auto& entry:path){
            door_navigation_expand(s,handle);TreeView_Expand(s.navigation,handle,TVE_EXPAND);HTREEITEM next{};
            for(const auto& item:d.navigation_items)if(TreeView_GetParent(s.navigation,item.handle)==handle &&
                item.entry.menu==entry.menu&&item.entry.number==entry.number&&item.entry.kind==entry.kind&&item.entry.target==entry.target){next=item.handle;break;}
            if(!next)break;handle=next;
        }
    }
    TreeView_SelectItem(s.navigation,handle);TreeView_EnsureVisible(s.navigation,handle);s.navigating=guard;
}
bool door_navigation_open(State& s,HTREEITEM handle){
    const auto node=door_navigation_item(s,handle);if(!node)return false;
    auto& d=s.doors;d.navigation_loading=true;
    struct Reset{bool& flag;~Reset(){flag=false;}} reset{d.navigation_loading};
    activate_page(s,26);if(!d.catalog)return true;
    const auto e=node->entry;d.navigation_placement.reset();
    if(handle==s.route_items[26]){d.menu=0;d.entry=0;door_fill_lists(s);door_show_view(s,DoorPage::categories);}
    else{
        const auto found=std::find_if(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& current){return current.menu==e.menu&&current.number==e.number&&current.kind==e.kind&&current.target==e.target;});
        if(found==d.catalog->entries.end()){d.menu=0;d.entry=0;door_fill_lists(s);door_show_view(s,DoorPage::categories);set_status(s,L"Door settings changed. Choose again from the refreshed tree.");}
        else if(e.kind==app::DoorEntryKind::submenu){d.menu=e.target;d.entry=0;d.navigation_placement=e;door_fill_lists(s);SendMessageW(d.entries,LB_SETCURSEL,static_cast<WPARAM>(-1),0);door_show_view(s,DoorPage::entries);}
        else{d.menu=e.menu;d.entry=e.number;d.door=e.target;door_fill_lists(s);door_show_view(s,DoorPage::entries);}
    }
    d.navigation_loading=false;door_navigation_sync(s);remember_page(s);return true;
}
std::optional<LRESULT> door_navigation_notify(State& s,const NMHDR* header){
    if(!header||header->hwndFrom!=s.navigation||s.navigating)return {};
    if(header->code==TVN_SELCHANGINGW&&s.doors.dragging)return TRUE;
    if(header->code==NM_CLICK&&!s.doors.dragging){const auto position=GetMessagePos();POINT point{GET_X_LPARAM(position),GET_Y_LPARAM(position)};ScreenToClient(s.navigation,&point);TVHITTESTINFO hit{};hit.pt=point;
        if(TreeView_HitTest(s.navigation,&hit)&&hit.hItem==TreeView_GetSelection(s.navigation)&&(hit.flags&(TVHT_ONITEMLABEL|TVHT_ONITEMICON)))if(const auto node=door_navigation_item(s,hit.hItem);node&&node->entry.kind==app::DoorEntryKind::submenu&&(s.doors.view==DoorPage::categories||s.doors.menu!=node->entry.target)){door_navigation_open(s,hit.hItem);return 0;}}
    if(header->code==TVN_BEGINDRAGW){const auto* event=reinterpret_cast<const NMTREEVIEWW*>(header);if(const auto item=door_navigation_item(s,event->itemNew.hItem))door_drag_begin(s,s.navigation,*item);return 0;}
    if(header->code==TVN_ITEMEXPANDINGW){const auto* event=reinterpret_cast<const NMTREEVIEWW*>(header);
        if(event->action==TVE_EXPAND&&door_navigation_item(s,event->itemNew.hItem)){
            if(!s.doors.catalog){s.doors.catalog=load_door_catalog(s);door_navigation_refresh(s);}
            door_navigation_expand(s,event->itemNew.hItem);return 0;
        }
    }
    if(header->code==TVN_ITEMEXPANDEDW){const auto* event=reinterpret_cast<const NMTREEVIEWW*>(header);const auto parent=event->itemNew.hItem;
        if(event->action==TVE_COLLAPSE&&door_navigation_item(s,parent)){
            auto& items=s.doors.navigation_items;s.navigating=true;
            items.erase(std::remove_if(items.begin(),items.end(),[&](const auto& item){for(auto h=TreeView_GetParent(s.navigation,item.handle);h;h=TreeView_GetParent(s.navigation,h))if(h==parent)return true;return false;}),items.end());
            while(const auto child=TreeView_GetChild(s.navigation,parent))TreeView_DeleteItem(s.navigation,child);s.navigating=false;return 0;
        }
    }
    return {};
}
