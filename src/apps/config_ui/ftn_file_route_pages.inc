void ftn_file_fill(State& s){
    auto& f=s.ftn;const auto* c=ftn_current(s);if(!c||f.network<0)return;f.populating=true;SendMessageW(f.files,LB_RESETCONTENT,0,0);SendMessageW(f.file_area,CB_RESETCONTENT,0,0);
    for(const auto& a:f.file_areas)if(a.area.kind==app::FileAreaKind::ordinary)ftn_combo_item(f.file_area,utf8_to_wide(a.area.name),a.area.id.value);
    const auto& n=c->networks[f.network];for(std::size_t i=0;i<c->file_echoes.size();++i)if(c->file_echoes[i].network==n.code){const auto title=utf8_to_wide(c->file_echoes[i].tag+"  "+c->file_echoes[i].description);const auto row=SendMessageW(f.files,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));SendMessageW(f.files,LB_SETITEMDATA,row,i);if(static_cast<int>(i)==f.file_echo)SendMessageW(f.files,LB_SETCURSEL,row,0);}
    const bool valid=f.file_echo>=0&&static_cast<std::size_t>(f.file_echo)<c->file_echoes.size()&&c->file_echoes[f.file_echo].network==n.code;
    for(const auto h:{f.file_tag,f.file_description,f.file_area})EnableWindow(h,valid&&f.catalog_ready);if(valid){const auto& e=c->file_echoes[f.file_echo];door_set(f.file_tag,e.tag);door_set(f.file_description,e.description);ftn_combo_select(f.file_area,e.file_area);}EnableWindow(GetDlgItem(s.pages[33],IDC_FTN_FILE_SAVE),valid&&f.catalog_ready);EnableWindow(GetDlgItem(s.pages[33],IDC_FTN_FILE_REMOVE),valid);f.populating=false;
}
bool collect_ftn_file(State& s,app::FtnConfiguration& c){
    auto& f=s.ftn;if(f.file_echo<0)return true;if(!f.catalog_ready||static_cast<std::size_t>(f.file_echo)>=c.file_echoes.size()||f.network<0||c.file_echoes[f.file_echo].network!=c.networks[f.network].code)return false;
    const auto area=ftn_combo_value(f.file_area);if(area<=0){set_status(s,L"Select an existing ordinary file area. Create file areas under Files > Groups & Areas first.");return false;}
    auto& e=c.file_echoes[f.file_echo];e.tag=ftn_fold(door_text(f.file_tag));e.description=door_text(f.file_description);e.file_area=static_cast<std::int64_t>(area);return true;
}
void ftn_file_edit(State& s,bool add){
    auto& f=s.ftn;auto c=*ftn_current(s);if(!collect_ftn_file(s,c)){ftn_list_select(f.files,f.file_echo);return;}
    if(add){app::FtnFileEcho e;e.network=c.networks[f.network].code;e.tag="NEW.FILES";f.file_echo=static_cast<int>(c.file_echoes.size());c.file_echoes.push_back(std::move(e));f.draft_changed=true;}else{const auto row=SendMessageW(f.files,LB_GETCURSEL,0,0);if(row==LB_ERR)return;f.file_echo=static_cast<int>(SendMessageW(f.files,LB_GETITEMDATA,row,0));}f.draft=std::move(c);ftn_file_fill(s);remember_page(s);SetFocus(f.file_tag);
}
void ftn_route_fill(State& s){
    auto& f=s.ftn;const auto* c=ftn_current(s);if(!c||f.network<0||static_cast<std::size_t>(f.network)>=c->networks.size())return;
    f.populating=true;SendMessageW(f.routes,LB_RESETCONTENT,0,0);SendMessageW(f.route_via,CB_RESETCONTENT,0,0);const auto& n=c->networks[f.network];
    SendMessageW(f.default_netmail_hub,CB_RESETCONTENT,0,0);
    ftn_combo_item(f.default_netmail_hub,L"None",0);
    std::int64_t default_selection{};
    for(std::size_t i=0;i<c->links.size();++i){
        const auto& link=c->links[i];
        if(link.network==n.code&&app::ftn_outbound(link))ftn_combo_item(f.route_via,utf8_to_wide(link.name),i);
        if(ftn_fold(link.network)!=ftn_fold(n.code)||!app::ftn_outbound(link))continue;
        const bool saved=ftn_fold(link.code)==ftn_fold(n.default_link);
        if(link.enabled||saved){
            const auto choice=static_cast<std::int64_t>(i+1);
            ftn_combo_item(f.default_netmail_hub,utf8_to_wide(link.name+" ("+link.address+")")+
                (link.enabled?L"":L" (disabled)"),choice);
            if(saved)default_selection=choice;
        }
    }
    ftn_combo_select(f.default_netmail_hub,default_selection);
    for(std::size_t i=0;i<c->routes.size();++i)if(c->routes[i].network==n.code){const auto link=std::find_if(c->links.begin(),c->links.end(),[&](const auto& l){return l.network==n.code&&l.code==c->routes[i].via_link;});const auto title=utf8_to_wide(c->routes[i].destination+"  via "+(link==c->links.end()?"Unavailable":link->name));const auto row=SendMessageW(f.routes,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));SendMessageW(f.routes,LB_SETITEMDATA,row,i);if(static_cast<int>(i)==f.route)SendMessageW(f.routes,LB_SETCURSEL,row,0);}
    const bool valid=f.route>=0&&static_cast<std::size_t>(f.route)<c->routes.size()&&c->routes[f.route].network==n.code;for(const auto h:{f.route_address,f.route_via,f.route_flavor})EnableWindow(h,valid);
    if(valid){const auto& r=c->routes[f.route];door_set(f.route_address,r.destination);SendMessageW(f.route_flavor,CB_SETCURSEL,static_cast<unsigned>(r.flavor),0);for(std::size_t i=0;i<c->links.size();++i)if(c->links[i].network==n.code&&c->links[i].code==r.via_link)ftn_combo_select(f.route_via,i);}EnableWindow(GetDlgItem(s.pages[34],IDC_FTN_ROUTE_SAVE),TRUE);EnableWindow(GetDlgItem(s.pages[34],IDC_FTN_ROUTE_REMOVE),valid);f.populating=false;
}
bool collect_ftn_default_hub(State& s,app::FtnConfiguration& c){
    const auto& f=s.ftn;
    if(f.network<0||static_cast<std::size_t>(f.network)>=c.networks.size())return false;
    auto& network=c.networks[f.network];
    const auto choice=ftn_combo_value(f.default_netmail_hub);
    if(choice==0){network.default_link.clear();return true;}
    if(choice>0&&static_cast<std::size_t>(choice)<=c.links.size()){
        const auto& link=c.links[static_cast<std::size_t>(choice-1)];
        if(ftn_fold(link.network)==ftn_fold(network.code)&&app::ftn_outbound(link)&&
           (link.enabled||(!network.enabled&&ftn_fold(link.code)==ftn_fold(network.default_link)))){
            network.default_link=link.code;
            return true;
        }
    }
    set_status(s,L"Not saved. Choose an enabled outgoing connection as the default NetMail hub, or choose None. Your edits remain.");
    return false;
}
bool collect_ftn_route(State& s,app::FtnConfiguration& c){
    auto& f=s.ftn;if(!collect_ftn_default_hub(s,c))return false;if(f.route<0)return true;if(static_cast<std::size_t>(f.route)>=c.routes.size())return false;const auto via=ftn_combo_value(f.route_via);const auto flavor=SendMessageW(f.route_flavor,CB_GETCURSEL,0,0);const auto index=static_cast<std::size_t>(via);
    if(via<0||index>=c.links.size()||c.links[index].network!=c.networks[f.network].code||!app::ftn_outbound(c.links[index])||flavor<0||flavor>2){set_status(s,L"Choose an outgoing connection in this network and a mail policy.");return false;}auto& r=c.routes[f.route];r.destination=door_text(f.route_address);r.via_link=c.links[index].code;r.flavor=static_cast<app::FtnMailFlavor>(flavor);return true;
}
void ftn_route_edit(State& s,bool add){auto& f=s.ftn;auto c=*ftn_current(s);if(!collect_ftn_route(s,c))return;if(add){app::FtnRoute r;r.network=c.networks[f.network].code;f.route=static_cast<int>(c.routes.size());c.routes.push_back(std::move(r));f.draft_changed=true;}else{const auto row=SendMessageW(f.routes,LB_GETCURSEL,0,0);if(row==LB_ERR)return;f.route=static_cast<int>(SendMessageW(f.routes,LB_GETITEMDATA,row,0));}f.draft=std::move(c);ftn_route_fill(s);remember_page(s);SetFocus(f.route_address);}
void create_ftn_file_routes(State& s){
    auto& f=s.ftn;FtnControls ui{s,s.pages[33]=page(s)};ui.label(L"File Echoes",12,8);ui.label(L"Map a file echo to an existing ordinary file area. Link access is explicit.",12,40,510,40);f.files=ui.list(12,92,510,160,IDC_FTN_FILE_LIST);ui.button(L"Add File Echo",12,264,240,IDC_FTN_FILE_ADD);ui.button(L"Edit File Echo",270,264,252,IDC_FTN_FILE_EDIT);
    f.file_tag=ui.edit(L"File echo tag",12,310,510,IDC_FTN_FILE_TAG,128);f.file_description=ui.edit(L"Description",12,372,510,IDC_FTN_FILE_DESCRIPTION,512);f.file_area=ui.combo(L"Local file area",12,434,510,IDC_FTN_FILE_AREA);ui.label(L"TIC import requires an authorized configured link. A blank TIC password also requires authenticated receipt from that same link.",12,500,510,60);
    ui.button(L"Save Changes",12,579,156,IDC_FTN_FILE_SAVE);ui.button(L"Remove Mapping...",180,579,174,IDC_FTN_FILE_REMOVE);ui.button(L"Back to Network",366,579,156,IDC_FTN_FILE_BACK);
    FtnControls route{s,s.pages[34]=page(s)};route.label(L"Routing & Polling",12,8);route.label(L"NetMail without a specific route or direct connection uses the default hub. Set poll intervals on each connection.",12,40,510,42);
    f.default_netmail_hub=route.combo(L"Default NetMail hub",12,89,510,IDC_FTN_DEFAULT_NETMAIL_HUB);
    route.label(L"Specific routes",12,145);
    f.routes=route.list(12,169,510,155,IDC_FTN_ROUTE_LIST);route.button(L"Add Route",12,337,156,IDC_FTN_ROUTE_ADD);route.button(L"Edit Route",180,337,156,IDC_FTN_ROUTE_EDIT);route.button(L"Remove Route...",348,337,174,IDC_FTN_ROUTE_REMOVE);
    f.route_address=route.edit(L"Destination FTN address",12,387,510,IDC_FTN_ROUTE_ADDRESS,96);f.route_via=route.combo(L"Send via connection",12,449,510,IDC_FTN_ROUTE_VIA);f.route_flavor=route.combo(L"Mail policy",12,511,510,IDC_FTN_ROUTE_FLAVOR);for(const auto title:{L"Normal",L"Hold",L"Crash"})SendMessageW(f.route_flavor,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));route.button(L"Save Changes",12,589,240,IDC_FTN_ROUTE_SAVE);route.button(L"Back to Network",270,589,252,IDC_FTN_ROUTE_BACK);
}
