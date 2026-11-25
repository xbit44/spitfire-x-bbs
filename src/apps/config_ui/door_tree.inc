// Native hierarchy and bounded owner-only list operations. No system clipboard
// secrets: this clipboard contains only stable menu/entry identities.
std::optional<DoorEditorState::TreeItem> door_tree_item(State& s,HTREEITEM handle){
    if(!handle)handle=TreeView_GetSelection(s.doors.categories);
    for(const auto& item:s.doors.tree_items)if(item.handle==handle)return item;return {};
}
std::vector<app::DoorMenuEntry> door_tree_entries(const app::DoorCatalog& c,app::DoorMenuId id){
    std::vector<app::DoorMenuEntry> result;for(const auto& e:c.entries)if(e.menu==id)result.push_back(e);
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.number<b.number;});return result;
}
HTREEITEM door_tree_add(State& s,HTREEITEM parent,app::DoorMenuEntry entry,bool unplaced=false,bool group=false){
    auto& d=s.doors;std::wstring label;
    if(group)label=L"Not on a menu";
    else if(entry.kind==app::DoorEntryKind::submenu){
        label=entry.number?std::to_wstring(entry.number)+L"  "+utf8_to_wide(entry.title):door_menu_name(d,entry.target);
        const auto rows=door_tree_entries(*d.catalog,entry.target);std::size_t doors{};for(const auto& e:rows)if(e.kind==app::DoorEntryKind::door)++doors;
        const auto children=rows.size()-doors;label+=L"    "+std::to_wstring(doors)+(doors==1?L" Door, ":L" Doors, ")+std::to_wstring(children)+(children==1?L" category":L" categories");
    }else{const auto* door=app::find_door(*d.catalog,entry.target);label=(entry.number?std::to_wstring(entry.number)+L"  ":L"")+utf8_to_wide(entry.title);if(door&&!app::door_has_command(*door))label+=L" [Unfinished]";else if(door&&!door->enabled)label+=L" [Disabled]";}
    TVINSERTSTRUCTW item{};item.hParent=parent;item.hInsertAfter=TVI_LAST;item.item.mask=TVIF_TEXT|TVIF_PARAM|TVIF_CHILDREN;
    item.item.pszText=label.data();item.item.lParam=group?65535:entry.kind==app::DoorEntryKind::submenu?entry.target:1000+entry.target;
    item.item.cChildren=group|| (entry.kind==app::DoorEntryKind::submenu&&!door_tree_entries(*d.catalog,entry.target).empty())?1:0;
    const auto handle=TreeView_InsertItem(d.categories,&item);d.tree_items.push_back({handle,std::move(entry),unplaced,group});return handle;
}
void door_tree_expand(State& s,HTREEITEM handle){
    auto& d=s.doors;if(!d.catalog||TreeView_GetChild(d.categories,handle))return;const auto node=door_tree_item(s,handle);if(!node)return;
    // Lazy branches keep the initial view small even with hundreds of Doors.
    if(node->group){
        for(const auto& m:d.catalog->menus)if(m.id&&!std::any_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::submenu&&e.target==m.id;}))door_tree_add(s,handle,{0,0,app::DoorEntryKind::submenu,m.id,m.name,{}},true);
        for(const auto& item:d.catalog->doors)if(!std::any_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==item.id;}))door_tree_add(s,handle,{0,0,app::DoorEntryKind::door,item.id,item.name,{}},true);
    }else if(node->entry.kind==app::DoorEntryKind::submenu){
        if(d.tree_items.size()>20000){set_status(s,L"Collapse other branches, or use Open Selected Item to view this category's bounded list.");return;}
        for(const auto& e:door_tree_entries(*d.catalog,node->entry.target))door_tree_add(s,handle,e);
    }
}
void door_fill_tree(State& s){
    auto& d=s.doors;TreeView_DeleteAllItems(d.categories);d.tree_items.clear();if(!d.catalog)return;
    const auto root=door_tree_add(s,TVI_ROOT,{0,0,app::DoorEntryKind::submenu,0,"Door Menu",{}});
    door_tree_expand(s,root);TreeView_Expand(d.categories,root,TVE_EXPAND);TreeView_SelectItem(d.categories,root);
    bool unused=false;
    for(const auto& m:d.catalog->menus)if(m.id&&!std::any_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::submenu&&e.target==m.id;}))unused=true;
    for(const auto& item:d.catalog->doors)if(!std::any_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==item.id;}))unused=true;
    if(unused)door_tree_add(s,TVI_ROOT,{},true,true);
}
void door_filter_definitions(State& s){
    auto& d=s.doors;const bool previous=d.populating;d.populating=true;
    auto query=text(d.find);if(!query.empty())CharLowerBuffW(query.data(),static_cast<DWORD>(query.size()));
    const auto selected=door_selected(d.definitions);SendMessageW(d.definitions,LB_RESETCONTENT,0,0);
    if(d.catalog){auto list=d.catalog->doors;std::stable_sort(list.begin(),list.end(),[](const auto& a,const auto& b){return a.name<b.name;});
        for(const auto& item:list){auto name=utf8_to_wide(item.name),match=name;if(!match.empty())CharLowerBuffW(match.data(),static_cast<DWORD>(match.size()));if(match.find(query)==std::wstring::npos)continue;
            const auto uses=std::count_if(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==item.id;});
            const auto label=name+(item.enabled?L"":L" [Disabled]")+(uses?L" - on "+std::to_wstring(uses)+(uses==1?L" menu":L" menus"):L" (not on a menu)");
            const auto row=SendMessageW(d.definitions,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));SendMessageW(d.definitions,LB_SETITEMDATA,row,item.id);if(selected&&*selected==item.id)SendMessageW(d.definitions,LB_SETCURSEL,row,0);
        }
    }
    if(SendMessageW(d.definitions,LB_GETCURSEL,0,0)==LB_ERR&&SendMessageW(d.definitions,LB_GETCOUNT,0,0)>0)SendMessageW(d.definitions,LB_SETCURSEL,0,0);
    d.populating=previous;
    if(!previous){const auto count=SendMessageW(d.definitions,LB_GETCOUNT,0,0);set_status(s,(L"Matching Doors: "+std::to_wstring(std::max<LRESULT>(0,count))+L". Clear restores the full list.").c_str());}
}
bool door_tree_paste(State& s,const DoorEditorState::TreeItem& target,int position,DoorPage back=DoorPage::categories){
    auto& d=s.doors;if(!d.catalog||!d.clipboard||target.group)return false;
    if(d.clipboard_revision!=d.catalog->revision)return door_problem(s,nullptr,L"The menu changed after Cut/Copy. Select the item and copy or cut it again.");
    const auto source=*d.clipboard;
    if(position==0&&target.entry.kind!=app::DoorEntryKind::submenu)position=1;
    const auto destination=position==0?target.entry.target:target.entry.menu;
    if(position!=0&&!target.entry.number)return door_problem(s,nullptr,L"Choose a placed item, or paste into a category.");
    if(!source.unplaced&&source.entry.menu==destination&&source.entry.number==target.entry.number&&position!=0)return true;
    std::vector<app::DoorConfigurationChange> steps;app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::save_entry;c.entry=source.entry;c.entry.menu=destination;
    const bool same=d.clipboard_cut&&!source.unplaced&&source.entry.menu==destination;
    if(!same){const auto existing=door_tree_entries(*d.catalog,destination);const auto last=existing.empty()?0U:static_cast<unsigned>(existing.back().number);const auto free=last<99?last+1:door_free_number(*d.catalog,destination);if(!free)return door_problem(s,nullptr,L"The destination already has 99 entries.");c.entry.number=static_cast<std::uint8_t>(free);c.create_only=true;}
    if(d.clipboard_cut&&!source.unplaced&&!same){c.move_entry=true;c.source_menu=source.entry.menu;c.previous_number=source.entry.number;}
    if(!d.clipboard_cut){
        app::DoorConfigurationChange copy;auto name=source.entry.title;if(name.size()>56)name.resize(56);name+=" - Copy";
        if(source.entry.kind==app::DoorEntryKind::submenu){const auto id=app::next_unused_door_menu_id(*d.catalog);if(!id)return door_problem(s,nullptr,L"There are already 999 categories.");copy.action=app::DoorConfigurationAction::duplicate_menu;copy.source_menu=source.entry.target;copy.menu={*id,name};c.entry.target=*id;}
        else{const auto* door=app::find_door(*d.catalog,source.entry.target);const auto id=app::next_unused_door_id(*d.catalog);if(!door||!id)return door_problem(s,nullptr,L"The Door is unavailable or there are already 999 Doors.");copy.action=app::DoorConfigurationAction::save_door;copy.create_only=true;copy.door=*door;copy.door.id=*id;copy.door.name=name;copy.door.enabled=false;c.entry.target=*id;}
        c.entry.title=name;steps.push_back(std::move(copy));
    }
    auto rows=door_tree_entries(*d.catalog,destination);
    for(const auto& e:rows)if(!same||e.number!=source.entry.number)c.order.push_back(e.number);
    auto at=c.order.end();if(position!=0){at=std::find(c.order.begin(),c.order.end(),target.entry.number);if(at==c.order.end())return false;if(position>0)++at;}
    c.order.insert(at,c.entry.number);steps.push_back(std::move(c));
    d.menu=destination;d.return_view=back;
    const bool saved=door_start_save(s,std::move(steps));if(saved)d.clipboard.reset();return saved;
}
bool door_tree_command(State& s,UINT id){
    auto& d=s.doors;if(!d.catalog||d.pending||!resolve_dirty(s))return true;const auto node=door_tree_item(s);if(!node||node->group)return true;
    if(id==IDC_DOOR_TREE_COPY||id==IDC_DOOR_TREE_CUT){
        if(node->entry.kind==app::DoorEntryKind::submenu&&!node->entry.target){set_status(s,L"Door Menu is the fixed root. Choose a category or Door.");return true;}
        d.clipboard=*node;d.clipboard_cut=id==IDC_DOOR_TREE_CUT;d.clipboard_revision=d.catalog->revision;set_status(s,d.clipboard_cut?L"Item cut for moving. Paste After an item or Paste Into a category; nothing has changed yet.":L"Item copied. Paste creates a separate copy; copied categories keep shared Door contents.");return true;
    }
    if(id==IDC_DOOR_TREE_PASTE||id==IDC_DOOR_TREE_PASTE_INTO){door_tree_paste(s,*node,id==IDC_DOOR_TREE_PASTE_INTO||!node->entry.number?0:1);return true;}
    if(id==IDC_DOOR_TREE_DELETE){
        if(node->entry.kind==app::DoorEntryKind::submenu){door_request_delete_category(s,node->entry.target,DoorPage::categories);return true;}
        if(!node->entry.number){set_status(s,L"Use Delete Category or All Configured Doors to delete an unused configuration. Door Menu cannot be deleted.");return true;}
        app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::remove_entry;c.entry=node->entry;
        door_confirm(s,c,DoorPage::categories,L"Remove \""+utf8_to_wide(node->entry.title)+L"\" from this menu?\r\n\r\nIts configuration, contents and other menu entries remain available.",L"Remove This Entry");return true;
    }
    if(id==IDC_DOOR_TREE_INSERT){
        HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,IDC_DOOR_MENU_NEW,L"Add Category");AppendMenuW(menu,MF_STRING,IDC_DOOR_TREE_ADD_DOOR,L"Add Door");POINT point{};GetCursorPos(&point);const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,s.window,nullptr);DestroyMenu(menu);if(command)door_configuration_command(s,command,BN_CLICKED);return true;
    }
    if(id==IDC_DOOR_TREE_ADD_DOOR){d.menu=node->entry.kind==app::DoorEntryKind::submenu?node->entry.target:node->entry.menu;door_configuration_command(s,IDC_DOOR_NEW,BN_CLICKED);return true;}
    return false;
}
void door_tree_context(State& s,POINT point){
    auto& d=s.doors;const auto node=door_tree_item(s);if(!node||node->group)return;
    HMENU menu=CreatePopupMenu();const auto movable=(node->entry.target||node->entry.kind==app::DoorEntryKind::door)?MF_ENABLED:MF_GRAYED;
    AppendMenuW(menu,MF_STRING|movable,IDC_DOOR_TREE_CUT,L"Cut\tCtrl+X");AppendMenuW(menu,MF_STRING|movable,IDC_DOOR_TREE_COPY,L"Copy\tCtrl+C");
    const auto paste=d.clipboard?MF_ENABLED:MF_GRAYED;
    AppendMenuW(menu,MF_STRING|paste,IDC_DOOR_TREE_PASTE,L"Paste After\tCtrl+V");
    if(node->entry.kind==app::DoorEntryKind::submenu)AppendMenuW(menu,MF_STRING|paste,IDC_DOOR_TREE_PASTE_INTO,L"Paste Into Category\tCtrl+Shift+V");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,IDC_DOOR_TREE_INSERT,L"Insert Item...\tInsert");
    const bool category=node->entry.kind==app::DoorEntryKind::submenu;
    AppendMenuW(menu,MF_STRING|((category?node->entry.target:node->entry.number)?MF_ENABLED:MF_GRAYED),IDC_DOOR_TREE_DELETE,category?L"Delete Category...\tDelete":L"Remove Menu Entry...\tDelete");
    const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,s.window,nullptr);DestroyMenu(menu);if(command)door_configuration_command(s,command,BN_CLICKED);
}
std::optional<LRESULT> door_configuration_notify(State& s,const NMHDR* header){
    if(header&&header->idFrom==IDC_DOOR_TABS&&header->code==TCN_SELCHANGE){
        const auto tab=TabCtrl_GetCurSel(s.doors.tabs);
        if(tab>=0&&tab<4){s.doors.selected_tab=static_cast<unsigned>(tab);door_show_view(s,s.doors.view);}
        return 0;
    }
    if(!header||header->hwndFrom!=s.doors.categories)return {};auto& d=s.doors;if(d.populating)return 0;
    if(header->code==TVN_ITEMEXPANDINGW){const auto* expand=reinterpret_cast<const NMTREEVIEWW*>(header);if(expand->action==TVE_EXPAND)door_tree_expand(s,expand->itemNew.hItem);return 0;}
    if(header->code==TVN_ITEMEXPANDEDW){const auto* event=reinterpret_cast<const NMTREEVIEWW*>(header);if(event->action==TVE_COLLAPSE){const auto parent=event->itemNew.hItem;
        d.tree_items.erase(std::remove_if(d.tree_items.begin(),d.tree_items.end(),[&](const auto& item){for(auto p=TreeView_GetParent(d.categories,item.handle);p;p=TreeView_GetParent(d.categories,p))if(p==parent)return true;return false;}),d.tree_items.end());
        while(const auto child=TreeView_GetChild(d.categories,parent))TreeView_DeleteItem(d.categories,child);
    }return 0;}
    if(header->code==NM_DBLCLK){door_open_category(s);return 0;}
    if(header->code==TVN_SELCHANGEDW){door_update_delete_actions(s);return 0;}
    if(header->code==TVN_BEGINDRAGW){const auto* event=reinterpret_cast<const NMTREEVIEWW*>(header);if(const auto node=door_tree_item(s,event->itemNew.hItem))door_drag_begin(s,d.categories,*node);return 0;}
    return {};
}
LRESULT CALLBACK door_tree_keys(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR context){
    auto& s=*reinterpret_cast<State*>(context);auto& d=s.doors;
    if(const auto result=door_drag_message(s,window,message,w,l))return *result;
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,door_tree_keys,id);
    if(message==WM_KEYDOWN&&w=='F'&&(GetKeyState(VK_CONTROL)&0x8000)&&d.view==DoorPage::definitions){SetFocus(d.find);SendMessageW(d.find,EM_SETSEL,0,-1);return 0;}
    if(window!=d.categories)return DefSubclassProc(window,message,w,l);
    if(message==WM_GETDLGCODE&&(w==VK_RETURN||w==VK_INSERT||w==VK_DELETE))return DefSubclassProc(window,message,w,l)|DLGC_WANTMESSAGE;
    if(message==WM_CONTEXTMENU){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(l==-1){RECT r{};TreeView_GetItemRect(window,TreeView_GetSelection(window),&r,TRUE);point={r.left,r.bottom};ClientToScreen(window,&point);}else{POINT client=point;ScreenToClient(window,&client);TVHITTESTINFO hit{};hit.pt=client;if(TreeView_HitTest(window,&hit))TreeView_SelectItem(window,hit.hItem);}door_tree_context(s,point);return 0;}
    if(message==WM_KEYDOWN){UINT command{};const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(ctrl&&w=='C')command=IDC_DOOR_TREE_COPY;else if(ctrl&&w=='X')command=IDC_DOOR_TREE_CUT;else if(ctrl&&w=='V')command=(GetKeyState(VK_SHIFT)&0x8000)?IDC_DOOR_TREE_PASTE_INTO:IDC_DOOR_TREE_PASTE;
        else if(w==VK_INSERT)command=IDC_DOOR_TREE_INSERT;else if(w==VK_DELETE)command=IDC_DOOR_TREE_DELETE;else if(w==VK_RETURN){door_open_category(s);return 0;}
        else if(w==VK_ESCAPE&&d.dragging){d.dragging=false;d.drag_target=nullptr;ReleaseCapture();TreeView_SetInsertMark(window,nullptr,FALSE);TreeView_SelectDropTarget(window,nullptr);return 0;}
        if(command){door_configuration_command(s,command,BN_CLICKED);return 0;}
    }
    return DefSubclassProc(window,message,w,l);
}
