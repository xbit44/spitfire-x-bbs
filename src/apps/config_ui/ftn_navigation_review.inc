void ftn_navigation_refresh(State& s){
    const auto root=s.route_items[29];if(!root||!s.ftn.config)return;const auto old=s.navigating;s.navigating=true;
    // A save may run inside TVN_SELCHANGING. Preserve existing node handles,
    // including the pending destination; update labels instead of rebuilding.
    for(const auto& entry:s.ftn.navigation){
        const bool exists=std::any_of(s.ftn.config->networks.begin(),s.ftn.config->networks.end(),[&](const auto& n){return n.code==entry.network;});
        if(!exists&&TreeView_GetParent(s.navigation,entry.handle)==root)TreeView_DeleteItem(s.navigation,entry.handle);
    }
    auto& entries=s.ftn.navigation;
    entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const auto& entry){return std::none_of(s.ftn.config->networks.begin(),s.ftn.config->networks.end(),[&](const auto& n){return n.code==entry.network;});}),entries.end());
    for(const auto& n:s.ftn.config->networks){auto title=utf8_to_wide(n.name+(n.enabled?"":" (disabled)"));
        const auto existing=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.network==n.code&&TreeView_GetParent(s.navigation,e.handle)==root;});
        if(existing!=entries.end()){TVITEMW item{};item.mask=TVIF_TEXT;item.hItem=existing->handle;item.pszText=title.data();TreeView_SetItem(s.navigation,&item);continue;}
        TVINSERTSTRUCTW item{};item.hParent=root;item.hInsertAfter=TVI_LAST;item.item.mask=TVIF_TEXT|TVIF_PARAM;item.item.pszText=title.data();item.item.lParam=30;const auto parent=TreeView_InsertItem(s.navigation,&item);entries.push_back({parent,n.code,30});
        for(const auto& child:std::array<std::pair<const wchar_t*,int>,5>{{{L"Connections",31},{L"Message Echoes",32},{L"File Echoes",33},{L"Routing & Polling",34},{L"Retention",30}}}){item.hParent=parent;item.item.pszText=const_cast<wchar_t*>(child.first);item.item.lParam=child.second;const auto handle=TreeView_InsertItem(s.navigation,&item);entries.push_back({handle,n.code,child.second});}
    }
    TVINSERTSTRUCTW held{};held.hParent=TreeView_GetParent(s.navigation,root);held.hInsertAfter=root;held.item.mask=TVIF_TEXT|TVIF_PARAM;held.item.pszText=const_cast<wchar_t*>(L"Held Mail");held.item.lParam=36;
    if(!s.ftn.held_item)s.ftn.held_item=TreeView_InsertItem(s.navigation,&held);TreeView_Expand(s.navigation,root,TVE_EXPAND);s.navigating=old;
}
bool ftn_navigation_open(State& s,HTREEITEM item){
    const auto found=std::find_if(s.ftn.navigation.begin(),s.ftn.navigation.end(),[&](const auto& n){return n.handle==item;});if(found==s.ftn.navigation.end())return false;const auto key=*found;if(!s.ftn.config)return true;
    const auto network=std::find_if(s.ftn.config->networks.begin(),s.ftn.config->networks.end(),[&](const auto& n){return n.code==key.network;});if(network==s.ftn.config->networks.end()){set_status(s,L"That network changed. Reload the named Networks list.");return true;}
    auto& f=s.ftn;f.network=static_cast<int>(network-s.ftn.config->networks.begin());f.link=f.echo=f.file_echo=f.route=f.permission_link=-1;f.wizard=false;f.draft=*f.config;f.draft_changed=false;activate_page(s,key.page);remember_page(s);return true;
}
bool ftn_stage_current(State& s,app::FtnConfiguration& c,bool later=false){
    switch(s.current_page){case 30:return collect_ftn_network(s,c,later);case 31:return collect_ftn_link(s,c);case 32:return collect_ftn_echo(s,c);case 33:return collect_ftn_file(s,c);case 34:return collect_ftn_route(s,c);default:return true;}
}
void ftn_open_section(State& s,int page){
    auto& f=s.ftn;const auto* current=ftn_current(s);if(!current||f.network<0)return;auto c=*current;if(!ftn_stage_current(s,c))return;f.draft=std::move(c);f.link=f.echo=f.file_echo=f.route=-1;
    activate_page(s,page);remember_page(s);
}
void ftn_review_fill(State& s){
    const auto* c=ftn_current(s);auto& f=s.ftn;if(!c||f.network<0)return;const auto& n=c->networks[f.network];std::size_t connections{},new_echoes{},existing{};for(const auto& l:c->links)if(l.network==n.code)++connections;for(const auto& e:c->message_echoes)if(e.network==n.code){if(e.store_locally&&!e.local_echo)++new_echoes;else ++existing;}
    const auto review=L"Network: "+utf8_to_wide(n.name)+L"\r\nYour address: "+utf8_to_wide(n.address)+L"\r\nConnections: "+(connections?std::to_wstring(connections):L"None configured (add later)")+L"\r\nNew local conferences: "+std::to_wstring(new_echoes)+L"\r\nExisting / pass-through echoes: "+std::to_wstring(existing)+L"\r\nOrigin line: "+utf8_to_wide(n.origin_line)+L"\r\n\r\nNo AreaFix subscription request is generated. Imported echoes do not mean a hub has accepted a subscription.\r\n\r\nSaved configuration and active transport are reported separately.";
    SetWindowTextW(f.review_text,review.c_str());auto active=*c;active.networks[f.network].enabled=true;const bool valid=app::validate_ftn_configuration(active).empty();f.populating=true;door_check(f.review_enable,f.enable_choice?n.enabled:valid);f.populating=false;remember_page(s);
}
void ftn_review_open(State& s){auto& f=s.ftn;const auto* current=ftn_current(s);if(!current||f.network<0)return;auto c=*current;if(!ftn_stage_current(s,c))return;f.draft=std::move(c);activate_page(s,37);ftn_review_fill(s);remember_page(s);}
void ftn_remove_network(State& s){
    auto& f=s.ftn;if(!f.config||!resolve_dirty(s))return;const auto row=SendMessageW(f.list,LB_GETCURSEL,0,0);if(row==LB_ERR){set_status(s,L"Select a network first.");return;}const auto index=static_cast<std::size_t>(SendMessageW(f.list,LB_GETITEMDATA,row,0));if(index>=f.config->networks.size())return;auto c=*f.config;const auto n=c.networks[index];
    const auto echoes=std::count_if(c.message_echoes.begin(),c.message_echoes.end(),[&](const auto& e){return e.network==n.code;});
    if(!ftn_confirm(s,L"Remove Network Settings",L"Remove "+utf8_to_wide(n.name)+L" and its FTN connection, route and echo mappings?\n\nMapped message echoes: "+std::to_wstring(echoes)+L". Local messages, file areas, files and preserved FTN spool data are retained. No mail is deleted by this settings action."))return;
    const auto remove=[&](auto& values){values.erase(std::remove_if(values.begin(),values.end(),[&](const auto& v){return v.network==n.code;}),values.end());};remove(c.links);remove(c.message_echoes);remove(c.file_echoes);remove(c.permissions);remove(c.routes);c.networks.erase(c.networks.begin()+static_cast<std::ptrdiff_t>(index));if(ftn_commit(s,c)){f.network=-1;activate_page(s,29);}
}
void create_ftn_review_held(State& s){
    auto& f=s.ftn;FtnControls ui{s,s.pages[37]=page(s)};ui.label(L"Review Network Setup",12,8);f.review_text=control(s,ui.parent,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,52,510,280,IDC_FTN_REVIEW_TEXT);f.review_enable=ui.check(L"Enable this network after setup",12,354,510,IDC_FTN_REVIEW_ENABLE);ui.button(L"Finish",12,408,156,IDC_FTN_REVIEW_FINISH);ui.button(L"Back",180,408,156,IDC_FTN_REVIEW_BACK);ui.button(L"Save and Finish Later",348,408,174,IDC_FTN_LATER);ui.button(L"Cancel Setup",12,456,156,IDC_FTN_SETUP_CANCEL);
    create_ftn_held(s);
}
