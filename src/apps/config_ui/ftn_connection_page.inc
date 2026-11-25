const app::FtnConfiguration* ftn_current(const State& s){return s.ftn.draft?&*s.ftn.draft:s.ftn.config?&*s.ftn.config:nullptr;}
void ftn_link_enable(State& s,bool enabled){auto& f=s.ftn;for(auto h:{f.link_name,f.link_address,f.link_use,f.link_host,f.link_port,f.link_enabled,f.link_tls,f.link_plain,f.link_poll,f.link_areafix,f.link_forward,f.link_aliases,f.link_tls_name,f.link_ca,f.link_provider,f.link_contact,GetDlgItem(s.pages[31],IDC_FTN_LINK_SAVE),GetDlgItem(s.pages[31],IDC_FTN_LINK_REMOVE)})EnableWindow(h,enabled);for(auto h:f.secrets)EnableWindow(h,enabled);for(auto h:f.clear)EnableWindow(h,enabled);}
void ftn_areafix_state(State& s){auto& f=s.ftn;const auto* c=ftn_current(s);if(!c||f.link<0||static_cast<std::size_t>(f.link)>=c->links.size())return;const auto& l=c->links[f.link];const auto selection=SendMessageW(f.link_areafix,CB_GETCURSEL,0,0);const bool set=!door_checked(f.clear[2])&&(SendMessageW(f.secrets[2],EM_GETMODIFY,0,0)?GetWindowTextLengthW(f.secrets[2])>0:!l.credentials.areafix.empty());const bool enabled=selection==1||(selection==0&&set);SetWindowTextW(f.link_areafix_status,enabled?L"AreaFix: On for explicitly permitted echoes. Separate password required.":L"AreaFix: Off. No automatic echo access is granted.");}
void ftn_link_fill(State& s){
    auto& f=s.ftn;const auto* c=ftn_current(s);if(!c)return;f.populating=true;SendMessageW(f.links,LB_RESETCONTENT,0,0);
    for(auto h:f.secrets){SetWindowTextW(h,L"");SendMessageW(h,EM_SETMODIFY,FALSE,0);}for(auto h:f.clear)door_check(h,false);ftn_link_enable(s,false);
    if(f.network<0||static_cast<std::size_t>(f.network)>=c->networks.size()){f.populating=false;return;}
    const auto& network=c->networks[f.network];
    for(std::size_t i=0;i<c->links.size();++i)if(c->links[i].network==network.code){const auto name=utf8_to_wide(c->links[i].name+(c->links[i].enabled?"":" (disabled)"));const auto row=SendMessageW(f.links,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(f.links,LB_SETITEMDATA,row,i);if(static_cast<int>(i)==f.link)SendMessageW(f.links,LB_SETCURSEL,row,0);}
    const bool available=f.link>=0&&static_cast<std::size_t>(f.link)<c->links.size()&&c->links[f.link].network==network.code;
    ftn_link_enable(s,available);
    SendMessageW(f.link_provider,CB_RESETCONTENT,0,0);ftn_combo_item(f.link_provider,L"No upstream provider",-1);for(std::size_t i=0;i<c->links.size();++i)if(static_cast<int>(i)!=f.link&&c->links[i].network==network.code&&app::ftn_outbound(c->links[i]))ftn_combo_item(f.link_provider,utf8_to_wide(c->links[i].name),i);ftn_combo_select(f.link_provider,-1);
    if(available){const auto& l=c->links[f.link];door_set(f.link_name,l.name);door_set(f.link_address,l.address);door_set(f.link_host,l.host);if(l.port)door_set_number(f.link_port,l.port);else door_set(f.link_port,"");door_check(f.link_contact,l.contact_on_mail);door_set_number(f.link_poll,l.poll_minutes);SendMessageW(f.link_use,CB_SETCURSEL,static_cast<unsigned>(l.use),0);door_check(f.link_enabled,l.enabled);door_check(f.link_tls,l.tls);door_check(f.link_plain,l.allow_legacy_plain_auth);door_check(f.link_forward,l.forward_missing_echoes);SendMessageW(f.link_areafix,CB_SETCURSEL,l.areafix_enabled?(*l.areafix_enabled?1:2):0,0);EnableWindow(f.link_host,app::ftn_outbound(l));EnableWindow(f.link_port,app::ftn_outbound(l));}
    if(available){const auto& l=c->links[f.link];std::string aliases;for(const auto& a:l.additional_addresses){if(!aliases.empty())aliases+=", ";aliases+=a;}door_set(f.link_aliases,aliases);door_set(f.link_tls_name,l.tls_server_name);door_set(f.link_ca,l.tls_ca_file);for(std::size_t i=0;i<c->links.size();++i)if(c->links[i].network==l.network&&c->links[i].code==l.forward_via_link)ftn_combo_select(f.link_provider,i);const std::array<const std::vector<std::uint8_t>*,4> secrets{{&l.credentials.session,&l.credentials.packet,&l.credentials.areafix,&l.credentials.tic}};for(std::size_t i=0;i<secrets.size();++i)SetWindowTextW(f.secret_status[i],secrets[i]->empty()?L"Current: Not set":L"Current: Set");SetWindowTextW(GetDlgItem(s.pages[31],IDC_FTN_LINK_SAVE),f.wizard?L"Keep in Setup Draft":L"Save Changes");ftn_areafix_state(s);}
    if(available){
        const auto& l=c->links[f.link];const std::array<const std::vector<std::uint8_t>*,4> stored{{&l.credentials.session,&l.credentials.packet,&l.credentials.areafix,&l.credentials.tic}};
        for(std::size_t i=0;i<stored.size();++i){
            if(!stored[i]->empty()){
                const auto secret=win32::unprotect_ftn_secret(s.root.u8string(),l.network,l.code,static_cast<win32::FtnCredentialKind>(i),*stored[i]);
                if(secret){std::array<wchar_t,129> wide{};std::size_t n{};for(unsigned char ch:secret->view())wide[n++]=ch;SetWindowTextW(f.secrets[i],wide.data());SecureZeroMemory(wide.data(),sizeof(wide));}
                else SetWindowTextW(f.secret_status[i],L"Current: Unable to read");
            }
            SendMessageW(f.secrets[i],EM_SETMODIFY,FALSE,0);
        }
    }
    if(available)ftn_areafix_state(s);
    f.populating=false;
}
bool collect_ftn_link(State& s,app::FtnConfiguration& c){
    auto& f=s.ftn;if(f.link<0||static_cast<std::size_t>(f.link)>=c.links.size())return true;
    if(f.network<0||static_cast<std::size_t>(f.network)>=c.networks.size()||c.links[f.link].network!=c.networks[f.network].code){set_status(s,L"Select a connection in this network.");return false;}
    auto& l=c.links[f.link];const auto use=SendMessageW(f.link_use,CB_GETCURSEL,0,0),areafix=SendMessageW(f.link_areafix,CB_GETCURSEL,0,0);unsigned port{},poll{};
    if(use<0||use>2||areafix<0||areafix>2||(!door_text(f.link_port).empty()&&!door_number(f.link_port,port,65535))||!door_number(f.link_poll,poll,65535)){set_status(s,L"Choose a connection direction and valid port/poll values. Your edits remain.");return false;}
    l.contact_on_mail=door_checked(f.link_contact);l.name=door_text(f.link_name);l.address=door_text(f.link_address);l.host=door_text(f.link_host);l.port=static_cast<std::uint16_t>(port);l.poll_minutes=static_cast<std::uint16_t>(poll);l.use=static_cast<app::FtnConnectionUse>(use);l.enabled=door_checked(f.link_enabled);l.tls=door_checked(f.link_tls);l.allow_legacy_plain_auth=door_checked(f.link_plain);l.forward_missing_echoes=door_checked(f.link_forward);l.areafix_enabled=areafix==0?std::optional<bool>{}:std::optional<bool>{areafix==1};
    l.additional_addresses=ftn_aliases(door_text(f.link_aliases));l.tls_server_name=door_text(f.link_tls_name);l.tls_ca_file=door_text(f.link_ca);const auto provider=ftn_combo_value(f.link_provider);l.forward_via_link=provider>=0&&static_cast<std::size_t>(provider)<c.links.size()?c.links[static_cast<std::size_t>(provider)].code:std::string{};
    const std::array<std::vector<std::uint8_t>*,4> fields{{&l.credentials.session,&l.credentials.packet,&l.credentials.areafix,&l.credentials.tic}};
    for(std::size_t i=0;i<fields.size();++i){
        const auto length=GetWindowTextLengthW(f.secrets[i]);const bool clear=door_checked(f.clear[i]);
        if(clear){fields[i]->clear();continue;}
        if(!SendMessageW(f.secrets[i],EM_GETMODIFY,0,0))continue;
        if(length<0||length>128){set_status(s,L"The password is too long. Your edits remain.");return false;}
        if(!length){fields[i]->clear();continue;}
        struct Erase {std::array<wchar_t,129> wide{};std::array<char,129> narrow{};~Erase(){SecureZeroMemory(wide.data(),sizeof(wide));SecureZeroMemory(narrow.data(),sizeof(narrow));}} secret;
        if(GetWindowTextW(f.secrets[i],secret.wide.data(),static_cast<int>(secret.wide.size()))!=length)return false;
        bool valid=true;for(int n=0;n<length;++n){const auto ch=secret.wide[static_cast<std::size_t>(n)];if(ch<32||ch>126){valid=false;break;}secret.narrow[static_cast<std::size_t>(n)]=static_cast<char>(ch);}
        if(!valid){set_status(s,L"FTN passwords use printable ASCII. Your edits remain.");return false;}
        const auto sealed=win32::protect_ftn_secret(s.root.u8string(),l.network,l.code,static_cast<win32::FtnCredentialKind>(i),std::string_view(secret.narrow.data(),static_cast<std::size_t>(length)));
        if(!sealed){set_status(s,L"The password could not be protected. Packet passwords are at most 8 bytes; other FTN passwords are at most 128. Your edits remain.");return false;}
        *fields[i]=*sealed;
    }
    return true;
}
bool stage_ftn_network(State& s){
    if(!s.ftn.config)return false;auto c=s.ftn.draft?*s.ftn.draft:*s.ftn.config;
    const bool changed=page_dirty(s);if(!collect_ftn_network(s,c,false))return false;s.ftn.draft=std::move(c);s.ftn.draft_changed=changed;return true;
}
void ftn_open_connections(State& s){
    if(s.current_page==30&&!stage_ftn_network(s))return;
    s.ftn.link=-1;activate_page(s,31);ftn_link_fill(s);remember_page(s);
}
void ftn_edit_link(State& s,bool add){
    auto& f=s.ftn;if(!f.config)return;
    auto c=f.draft?*f.draft:*f.config;
    if(f.network<0||static_cast<std::size_t>(f.network)>=c.networks.size())return;
    if(f.link>=0&&s.current_page==31&&!collect_ftn_link(s,c)){ftn_list_select(f.links,f.link);return;}
    if(add){app::FtnLink l;l.code=ftn_new_code();l.network=c.networks[f.network].code;l.name="New Connection";f.link=static_cast<int>(c.links.size());c.links.push_back(std::move(l));f.draft_changed=true;}
    else{const auto row=SendMessageW(f.links,LB_GETCURSEL,0,0);if(row==LB_ERR)return;f.link=static_cast<int>(SendMessageW(f.links,LB_GETITEMDATA,row,0));}
    f.draft=std::move(c);ftn_link_fill(s);remember_page(s);SetFocus(f.link_name);
}
void ftn_link_tab(State& s){
    auto& f=s.ftn;const auto selected=TabCtrl_GetCurSel(f.link_tabs);SendMessageW(s.pages[31],WM_SETREDRAW,FALSE,0);
    for(std::size_t i=0;i<f.link_groups.size();++i)for(HWND h:f.link_groups[i])ShowWindow(h,selected==static_cast<int>(i)?SW_SHOW:SW_HIDE);
    SendMessageW(s.pages[31],WM_SETREDRAW,TRUE,0);InvalidateRect(s.pages[31],nullptr,TRUE);
}
void create_ftn_connections(State& s){
    auto& f=s.ftn;const auto p=s.pages[31]=page(s);int group=-1;
    const auto track=[&](HWND h){if(group>=0)f.link_groups[static_cast<std::size_t>(group)].push_back(h);return h;};
    const auto caption=[&](const wchar_t* title,int x,int y,int width){return track(label(s,p,title,x,y,width,20));};
    const auto button=[&](const wchar_t* title,int x,int y,int width,int id){return track(control(s,p,L"BUTTON",title,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,27,id));};
    const auto check=[&](const wchar_t* title,int x,int y,int width,int id){return track(control(s,p,L"BUTTON",title,WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,25,id));};
    const auto edit=[&](const wchar_t* title,int x,int y,int width,int id,unsigned max,bool password=false){caption(title,x,y,width);auto h=track(control(s,p,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|(password?ES_PASSWORD:0),x,y+22,width,25,id));SendMessageW(h,EM_SETLIMITTEXT,max,0);return h;};
    caption(L"Connections",12,8,510);caption(L"A connection may be an uplink or downlink. The first one is optional.",12,40,510);
    f.links=control(s,p,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,12,82,510,112,IDC_FTN_LINK_LIST);
    button(L"Add Connection",12,205,156,IDC_FTN_LINK_ADD);button(L"Edit Connection",180,205,156,IDC_FTN_LINK_EDIT);button(L"Remove...",348,205,174,IDC_FTN_LINK_REMOVE);
    f.link_tabs=control(s,p,WC_TABCONTROLW,L"",WS_TABSTOP,12,247,510,28,IDC_FTN_LINK_TABS);
    for(auto title:{L"Connection",L"Passwords",L"AreaFix",L"Advanced"}){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(title);TabCtrl_InsertItem(f.link_tabs,TabCtrl_GetItemCount(f.link_tabs),&item);}
    group=0;
    f.link_name=edit(L"Connection name",12,294,240,IDC_FTN_LINK_NAME,96);f.link_address=edit(L"Remote FTN address",270,294,252,IDC_FTN_LINK_ADDRESS,96);
    caption(L"Connection use",12,357,240);f.link_use=track(control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,12,379,510,120,IDC_FTN_LINK_USE));for(const auto title:{L"Inbound only",L"Outbound only",L"Inbound and outbound"})SendMessageW(f.link_use,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    f.link_host=edit(L"Outgoing host (not required for inbound only)",12,423,340,IDC_FTN_LINK_HOST,253);f.link_port=edit(L"Port (blank: nodelist)",370,423,152,IDC_FTN_LINK_PORT,5);
    f.link_aliases=edit(L"Allowed additional remote FTN addresses (comma separated)",12,485,510,IDC_FTN_LINK_ALIASES,3200);f.link_enabled=check(L"Enabled",12,547,510,IDC_FTN_LINK_ENABLED);
    group=1;
    const std::array<const wchar_t*,4> names{{L"Session password",L"Packet password",L"AreaFix password",L"TIC password"}};
    for(std::size_t i=0;i<names.size();++i){const int y=294+static_cast<int>(i)*70;f.secrets[i]=edit(names[i],12,y,340,IDC_FTN_SESSION_SECRET+static_cast<int>(i),i==1?8:128);f.secret_status[i]=caption(L"Current: Not set",370,y,152);f.clear[i]=check(L"Remove current",370,y+22,152,IDC_FTN_SESSION_CLEAR+static_cast<int>(i));}
    group=2;
    caption(L"Allow this link to manage permitted message echoes (AreaFix)",12,294,510);f.link_areafix=track(control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,12,322,510,120,IDC_FTN_LINK_AREAFIX));for(const auto title:{L"Use separate-password default",L"Enabled",L"Disabled"})SendMessageW(f.link_areafix,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    f.link_areafix_status=caption(L"Default: Off without its separate password; On when the password is set.",12,365,510);caption(L"An explicitly saved Disabled choice stays Disabled when passwords change.",12,395,510);
    f.link_forward=check(L"Request missing echoes from a configured upstream provider",12,430,510,IDC_FTN_LINK_FORWARD);caption(L"Upstream connection (uses its separate AreaFix password)",12,469,510);f.link_provider=track(control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,12,491,510,160,IDC_FTN_LINK_PROVIDER));
    group=3;
    button(L"Echo Access Exceptions...",12,590,300,IDC_FTN_PERMISSIONS);
    f.link_poll=edit(L"Poll interval in minutes (0 = timer off)",12,294,510,IDC_FTN_LINK_POLL,5);
    f.link_contact=check(L"Contact this connection when outgoing mail is queued",12,550,510,IDC_FTN_LINK_CONTACT);
    f.link_tls=check(L"Use explicitly configured verified TLS",12,354,510,IDC_FTN_LINK_TLS);f.link_plain=check(L"Allow legacy plain authentication for this link",12,389,510,IDC_FTN_LINK_PLAIN);
    f.link_tls_name=edit(L"TLS server name (blank uses outgoing host)",12,426,510,IDC_FTN_LINK_TLS_NAME,253);f.link_ca=edit(L"Additional trusted CA file (blank uses built-in trust)",12,488,510,IDC_FTN_LINK_CA,1024);
    group=-1;button(L"Save Changes",12,642,240,IDC_FTN_LINK_SAVE);button(L"Back to Network",270,642,252,IDC_FTN_LINK_BACK);
    ftn_link_enable(s,false);ftn_link_tab(s);
}
