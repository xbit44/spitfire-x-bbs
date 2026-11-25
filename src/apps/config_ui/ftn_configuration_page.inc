#include "ftn_configuration_client.inc"
#include "ftn_editor_controls.inc"
void ftn_navigation_refresh(State&);

void ftn_listener_controls(State& s){
    const auto& f=s.ftn;const auto mode=SendMessageW(f.mode,CB_GETCURSEL,0,0);const bool native=mode==0;
    for(const auto h:{f.listener,f.bind,f.tls_listener})EnableWindow(h,native);
    EnableWindow(f.port,native&&door_checked(f.listener));EnableWindow(f.program,mode==1);EnableWindow(GetDlgItem(s.pages[29],IDC_FTN_PROGRAM_BROWSE),mode==1);
    const bool tls=native&&door_checked(f.tls_listener);
    for(const auto h:{f.tls_port,f.tls_certificate,f.tls_key})EnableWindow(h,tls);
}
void ftn_fill(State& s){
    auto& f=s.ftn;f.populating=true;SendMessageW(f.list,LB_RESETCONTENT,0,0);
    if(f.config){
        for(std::size_t i=0;i<f.config->networks.size();++i){const auto& n=f.config->networks[i];const auto name=utf8_to_wide(n.name+(n.enabled?"":" (disabled)"));const auto row=SendMessageW(f.list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(f.list,LB_SETITEMDATA,row,i);}
        SendMessageW(f.mode,CB_SETCURSEL,static_cast<unsigned>(f.config->settings.mode),0);door_set(f.program,f.config->settings.external_executable);door_check(f.listener,f.config->settings.listener_enabled);door_set(f.bind,f.config->settings.bind_address);door_set_number(f.port,f.config->settings.port);door_check(f.direct,f.config->settings.allow_direct_local_netmail);door_set(f.tls_certificate,f.config->settings.tls_certificate);door_set(f.tls_key,f.config->settings.tls_private_key);door_check(f.tls_listener,f.config->settings.tls_listener_enabled);door_set_number(f.tls_port,f.config->settings.tls_port);
    }
    ftn_listener_controls(s);f.populating=false;ftn_navigation_refresh(s);
}
void refresh_ftn(State& s){
    s.ftn.config=query_ftn(s);s.ftn.draft.reset();s.ftn.draft_changed=false;s.ftn.import_base.reset();s.ftn.import_changed=false;ftn_fill(s);
    set_status(s,s.ftn.config?L"Saved FTN settings loaded.":L"FTN settings could not be loaded. No changes were made.");
}
std::string ftn_new_code(){const auto token=ftn_token();constexpr char hex[]="0123456789abcdef";std::string out="n-";for(auto b:token){out+=hex[b>>4];out+=hex[b&15];}return out;}
bool ftn_commit(State& s,const app::FtnConfiguration& value){
    s.ftn.last_save_conflict=false;
    const auto issues=app::validate_ftn_configuration(value);
    if(!issues.empty()){
        const auto& issue=issues.front();std::wstring message=L"Check the network, connection or echo settings before saving.";
        if(issue.field=="network.address")message=L"Enter a complete FTN address before enabling the network, or choose Save and Finish Later.";
        else if(issue.field=="network.default_hub")message=L"In Routing & Polling, clear the default NetMail hub or choose another enabled outgoing connection.";
        else if(issue.field=="network")message=L"Enter a network name and a valid HTTP or HTTPS EchoList URL, or save the unfinished network for later.";
        else if(issue.field=="link.endpoint")message=L"An enabled outgoing connection needs a destination host and port.";
        else if(issue.field=="link.areafix")message=L"AreaFix needs its separate password before it can be enabled.";
        else if(issue.field=="catalog")message=L"The configuration exceeds the available capacity. Reduce the selected entries.";
        else if(issue.field=="echo.local_name")message=L"Enter a local conference name of at most 64 characters.";
        else if(issue.field=="mailer.tls_listener")message=L"Enter a TLS certificate and private key before enabling the TLS listener.";
        else if(issue.field=="network.msg_directory")message=L"Enter the full .MSG NetMail directory before enabling its scanner.";
        else if(issue.field=="mailer.tls_port")message=L"Choose different ports for the classic and TLS listeners when both are enabled.";
        set_status(s,L"Not saved. "+message+L" Your edits remain.");return false;
    }
    const auto result=apply_ftn(s,value);
    s.ftn.last_save_conflict=result.code==app::FtnConfigurationCode::conflict;
    if(result.cancelled){set_status(s,L"Retention review cancelled. Saved settings and protection dates are unchanged. Your edits remain unsaved.");return false;}
    if(result.code!=app::FtnConfigurationCode::ok){for(int i=29;i<39;++i)EnableWindow(GetDlgItem(s.pages[i],IDC_FTN_RECONCILE),s.ftn.pending.has_value());set_status(s,result.code==app::FtnConfigurationCode::conflict?L"Settings or message/file areas changed elsewhere. Reload and review before saving. Your edits remain.":result.code==app::FtnConfigurationCode::busy?L"The configuration writer is busy or this import exceeds its available capacity. Your edits remain.":result.code==app::FtnConfigurationCode::invalid?L"Not saved. Check the FTN settings and protected credentials. Your edits remain.":s.ftn.pending?L"Save was not confirmed. Choose Check Save Result before another save or navigation. Your edits remain.":L"The configuration service is unavailable. Your edits remain.");return false;}
    if(!result.configuration){set_status(s,L"Save confirmed at revision "+std::to_wstring(result.applied_revision)+L". Reload to view the latest settings.");return false;}
    const auto* old=s.ftn.draft?&*s.ftn.draft:&*s.ftn.config;const auto key=s.ftn.network>=0&&static_cast<std::size_t>(s.ftn.network)<old->networks.size()?old->networks[s.ftn.network].code:std::string{};
    s.ftn.config=result.configuration;s.ftn.draft.reset();s.ftn.draft_changed=false;s.ftn.network=-1;for(std::size_t i=0;i<s.ftn.config->networks.size();++i)if(s.ftn.config->networks[i].code==key)s.ftn.network=static_cast<int>(i);ftn_fill(s);remember_page(s);
    set_status(s,s.mode==State::ConnectionMode::offline?L"FTN settings saved for the next BBS start.":L"FTN settings saved.");return true;
}
void ftn_network_fill(State& s){
    auto& f=s.ftn;const auto& c=f.draft?*f.draft:*f.config;if(f.network<0||static_cast<std::size_t>(f.network)>=c.networks.size())return;
    f.populating=true;const auto& n=c.networks[f.network];SendMessageW(f.net_source,CB_SETCURSEL,static_cast<unsigned>(n.echo_list_source),0);door_set(f.net_file,n.echo_list_file);door_set(f.node_file,n.nodelist_file);SendMessageW(f.net_msg,CB_SETCURSEL,n.msg_netmail_enabled.value_or(false)?1:0,0);door_set(f.net_msg_path,n.msg_netmail_directory);door_set(f.name,n.name);door_set(f.network_description,n.description);door_set(f.address,n.address);door_set(f.origin,n.origin_line);door_set(f.url,n.echo_list_url);door_check(f.enabled,n.enabled);door_check(f.keep,n.keep_processed);door_set_number(f.days,n.retention_days);
    std::string aliases;for(const auto& a:n.additional_addresses){if(!aliases.empty())aliases+=", ";aliases+=a;}door_set(f.aliases,aliases);EnableWindow(f.days,n.keep_processed);
    SetWindowTextW(f.detail,(L"Network: "+utf8_to_wide(n.name)+L". A hub or first connection is optional.").c_str());ShowWindow(f.preset,f.wizard?SW_SHOW:SW_HIDE);f.populating=false;
}
void ftn_open_network(State& s,bool add,bool wizard=false){
    if(!s.ftn.config||!resolve_dirty(s))return;auto& f=s.ftn;
    if(add){f.draft=*f.config;app::FtnNetwork n;n.code=ftn_new_code();n.name="New Network";n.origin_line=s.configuration.saved.bbs_name;f.network=static_cast<int>(f.draft->networks.size());f.draft->networks.push_back(std::move(n));f.draft_changed=true;}
    else{const auto row=SendMessageW(f.list,LB_GETCURSEL,0,0);if(row==LB_ERR){set_status(s,L"Select a network first.");return;}f.network=static_cast<int>(SendMessageW(f.list,LB_GETITEMDATA,row,0));f.draft=*f.config;f.draft_changed=false;}
    f.wizard=wizard;f.enable_choice=false;f.link=f.echo=f.file_echo=f.route=f.permission_link=-1;activate_page(s,30);ftn_network_fill(s);remember_page(s);SetFocus(f.name);
}
std::vector<std::string> ftn_aliases(std::string text){
    std::vector<std::string> out;std::size_t begin{};while(begin<text.size()){const auto end=text.find(',',begin);auto s=text.substr(begin,end==std::string::npos?std::string::npos:end-begin);while(!s.empty()&&s.front()==' ')s.erase(s.begin());while(!s.empty()&&s.back()==' ')s.pop_back();if(!s.empty())out.push_back(std::move(s));if(end==std::string::npos)break;begin=end+1;}return out;
}
bool collect_ftn_network(State& s,app::FtnConfiguration& next,bool later){
    auto& f=s.ftn;if(f.network<0||static_cast<std::size_t>(f.network)>=next.networks.size())return false;auto& n=next.networks[f.network];unsigned days{};
    if(!door_number(f.days,days,65535)){set_status(s,L"Enter retention days from 0 to 65535. Zero keeps processed originals forever.");return false;}
    n.name=door_text(f.name);n.description=door_text(f.network_description);n.address=door_text(f.address);n.additional_addresses=ftn_aliases(door_text(f.aliases));n.origin_line=door_text(f.origin);n.echo_list_url=door_text(f.url);n.echo_list_source=SendMessageW(f.net_source,CB_GETCURSEL,0,0)==1?app::FtnEchoListSource::local_file:app::FtnEchoListSource::url;n.echo_list_file=door_text(f.net_file);n.nodelist_file=door_text(f.node_file);const auto msg=SendMessageW(f.net_msg,CB_GETCURSEL,0,0);n.msg_netmail_enabled=msg==1;n.msg_netmail_directory=door_text(f.net_msg_path);n.enabled=later?false:door_checked(f.enabled);n.keep_processed=door_checked(f.keep);n.retention_days=static_cast<std::uint16_t>(days);return true;
}
#include "ftn_connection_page.inc"
#include "ftn_echo_page.inc"
#include "ftn_file_route_pages.inc"
#include "ftn_permissions_page.inc"
#include "ftn_held_page.inc"
#include "ftn_navigation_review.inc"
#include "ftn_import_page.inc"
#include "ftn_setup_page.inc"
bool save_ftn(State& s,bool later=false){
    auto& f=s.ftn;if(!f.config)return false;
    if(f.setup.clone){ns_error(s,L"Return from Clone Options to review and save the draft.");return false;}
    if(f.setup.active){if(f.setup.step==9&&!f.setup.clone)return ns_save(s,false);ns_error(s,L"Finish the setup review before saving. Use Cancel Setup to discard it.");return false;}
    if(s.current_page==38&&f.permission_edits&&!ftn_permissions_apply(s))return false;
    auto next=f.draft?*f.draft:*f.config;
    if(s.current_page==29){
        const auto mode=SendMessageW(f.mode,CB_GETCURSEL,0,0);
        if(mode<0||mode>2){set_status(s,L"Not saved. Choose a mailer. Your edits remain.");return false;}
        const bool classic=door_checked(f.listener),tls=door_checked(f.tls_listener);
        unsigned port=next.settings.port,tls_port=next.settings.tls_port;
        if(mode==0&&classic&&(!door_number(f.port,port,65535)||!port)){set_status(s,L"Not saved. Enter a classic BinkP port from 1 to 65535. Your edits remain.");return false;}
        if(mode==0&&tls&&(!door_number(f.tls_port,tls_port,65535)||!tls_port)){set_status(s,L"Not saved. Enter a TLS port from 1 to 65535. Your edits remain.");return false;}
        next.settings.mode=static_cast<app::FtnMailerMode>(mode);next.settings.external_executable=door_text(f.program);next.settings.listener_enabled=classic;next.settings.bind_address=door_text(f.bind);next.settings.port=static_cast<std::uint16_t>(port);next.settings.allow_direct_local_netmail=door_checked(f.direct);
        // Inactive listener controls do not replace saved settings or block Save.
        if(tls){next.settings.tls_certificate=door_text(f.tls_certificate);next.settings.tls_private_key=door_text(f.tls_key);}
        next.settings.tls_listener_enabled=tls;next.settings.tls_port=static_cast<std::uint16_t>(tls_port);
    }else if(!ftn_stage_current(s,next,later))return false;
    if(later&&f.network>=0)next.networks[f.network].enabled=false;
    if(s.current_page==37&&f.network>=0)next.networks[f.network].enabled=later?false:door_checked(f.review_enable);
    if(f.wizard&&!later&&s.current_page!=37){f.draft=next;ftn_review_open(s);return false;}
    const auto updates=std::count_if(next.message_echoes.begin(),next.message_echoes.end(),[](const auto& e){return e.update_local_name||e.update_local_permissions;});
    if(updates&&!ftn_confirm(s,L"Review Conference Changes",L"Apply reviewed name, security or pipe-color changes to "+std::to_wstring(updates)+L" existing local conferences? Message contents are preserved."))return false;
    if(!ftn_commit(s,next))return false;
    if(s.current_page==30){f.draft=*f.config;ftn_network_fill(s);remember_page(s);}
    if(s.current_page==31){f.draft=*f.config;ftn_link_fill(s);remember_page(s);}
    if(s.current_page==32){f.draft=*f.config;ftn_catalogs(s);ftn_echo_fill(s);remember_page(s);}
    if(s.current_page==33){f.draft=*f.config;ftn_file_fill(s);remember_page(s);}
    if(s.current_page==34){f.draft=*f.config;ftn_route_fill(s);remember_page(s);}
    if(s.current_page==38){f.draft=*f.config;ftn_permissions_fill(s);remember_page(s);}
    if(s.current_page==37){f.wizard=false;f.draft=*f.config;activate_page(s,30);ftn_network_fill(s);remember_page(s);}return true;
}
void discard_ftn(State& s){ns_end(s);s.ftn.draft.reset();s.ftn.draft_changed=false;s.ftn.import_base.reset();s.ftn.import_changed=false;ftn_fill(s);if(s.current_page>=30&&s.current_page!=36&&(!s.ftn.config||s.ftn.network<0||static_cast<std::size_t>(s.ftn.network)>=s.ftn.config->networks.size())){activate_page(s,29);return;}if(s.current_page==30)ftn_network_fill(s);if(s.current_page==31)ftn_link_fill(s);if(s.current_page==32)ftn_echo_fill(s);if(s.current_page==33)ftn_file_fill(s);if(s.current_page==34)ftn_route_fill(s);if(s.current_page==37)ftn_review_fill(s);if(s.current_page==38)ftn_permissions_fill(s);}
void ftn_remove_item(State& s,int kind){
    auto& f=s.ftn;auto c=*ftn_current(s);const int selected=kind==0?f.link:kind==1?f.echo:kind==2?f.file_echo:f.route;if(selected<0)return;
    if(kind==3&&!collect_ftn_default_hub(s,c))return;
    if(!ftn_confirm(s,L"Remove FTN Settings",L"Remove the selected FTN connection, echo mapping or route?\n\nDependent FTN permissions/routes are removed as needed. Local messages, conferences, files and preserved spool data remain."))return;
    const auto network=c.networks[f.network].code;std::string tag,link;
    if(kind==0){if(static_cast<std::size_t>(selected)>=c.links.size())return;link=c.links[selected].code;if(c.networks[f.network].default_link==link)c.networks[f.network].default_link.clear();c.links.erase(c.links.begin()+selected);c.routes.erase(std::remove_if(c.routes.begin(),c.routes.end(),[&](const auto& r){return r.network==network&&r.via_link==link;}),c.routes.end());for(auto& l:c.links)if(l.network==network&&l.forward_via_link==link){l.forward_via_link.clear();l.forward_missing_echoes=false;}}
    else if(kind==1){if(static_cast<std::size_t>(selected)>=c.message_echoes.size())return;tag=c.message_echoes[selected].tag;c.message_echoes.erase(c.message_echoes.begin()+selected);}
    else if(kind==2){if(static_cast<std::size_t>(selected)>=c.file_echoes.size())return;tag=c.file_echoes[selected].tag;c.file_echoes.erase(c.file_echoes.begin()+selected);}
    else{if(static_cast<std::size_t>(selected)>=c.routes.size())return;c.routes.erase(c.routes.begin()+selected);}
    c.permissions.erase(std::remove_if(c.permissions.begin(),c.permissions.end(),[&](const auto& p){return p.network==network&&((!link.empty()&&p.link==link)||(!tag.empty()&&p.tag==tag&&p.file_echo==(kind==2)));}),c.permissions.end());
    f.link=f.echo=f.file_echo=f.route=-1;f.draft=std::move(c);f.draft_changed=true;if(!f.wizard&&!ftn_commit(s,*f.draft))return;if(f.config&&!f.draft)f.draft=*f.config;const auto page=s.current_page;activate_page(s,page);remember_page(s);
}
bool ftn_configuration_command(State& s,int id,int notification){
    if(id<IDC_FTN_LIST||id>=IDC_FTN_LAST)return false;
    if(ftn_held_command(s,id,notification))return true;
    if(ns_command(s,id,notification))return true;
    if(s.ftn.pending&&id!=IDC_FTN_RECONCILE){set_status(s,L"Choose Check Save Result to recover this exact FTN save before another action.");return true;}
    if(!s.ftn.populating&&s.current_page==35){
        const auto h=GetDlgItem(s.pages[35],id);wchar_t cls[32]{};if(h)GetClassNameW(h,cls,32);
        if((_wcsicmp(cls,L"Edit")==0&&notification==EN_CHANGE&&id!=IDC_FTN_IMPORT_SEARCH)||(_wcsicmp(cls,L"ComboBox")==0&&notification==CBN_SELCHANGE)||(id==IDC_FTN_IMPORT_RENAME&&notification==BN_CLICKED))s.ftn.import_changed=true;
    }
    if(!s.ftn.populating&&s.current_page>=30&&s.current_page!=35&&s.current_page!=36){
        const auto h=GetDlgItem(s.pages[s.current_page],id);wchar_t cls[32]{};if(h)GetClassNameW(h,cls,32);
        const bool permission_box=id==IDC_FTN_PERMISSION_RECEIVE||id==IDC_FTN_PERMISSION_SEND||id==IDC_FTN_PERMISSION_MANAGE;
        const bool edit=_wcsicmp(cls,L"Edit")==0&&notification==EN_CHANGE&&id!=IDC_FTN_ECHO_FIND;
        if(edit&&id>=IDC_FTN_SESSION_SECRET&&id<=IDC_FTN_TIC_SECRET){SendMessageW(h,EM_SETMODIFY,TRUE,0);door_check(s.ftn.clear[static_cast<std::size_t>(id-IDC_FTN_SESSION_SECRET)],false);}
        const bool combo=_wcsicmp(cls,L"ComboBox")==0&&notification==CBN_SELCHANGE&&id!=IDC_FTN_PERMISSION_LINK;
        const bool box=_wcsicmp(cls,L"Button")==0&&(GetWindowLongPtrW(h,GWL_STYLE)&BS_TYPEMASK)==BS_AUTOCHECKBOX&&notification==BN_CLICKED;
        if(permission_box&&box)s.ftn.permission_edits=true;
        else if(edit||combo||box)s.ftn.draft_changed=true;
    }
    if(!s.ftn.populating&&s.current_page==30){
        if(id==IDC_FTN_ENABLED&&notification==BN_CLICKED)s.ftn.enable_choice=true;
        if(notification==EN_CHANGE&&s.ftn.wizard&&!s.ftn.enable_choice&&s.ftn.draft&&s.ftn.network>=0){
            auto c=*s.ftn.draft;auto& n=c.networks[s.ftn.network];n.name=door_text(s.ftn.name);n.address=door_text(s.ftn.address);n.additional_addresses=ftn_aliases(door_text(s.ftn.aliases));n.echo_list_url=door_text(s.ftn.url);n.enabled=true;
            door_check(s.ftn.enabled,app::validate_ftn_configuration(c).empty());
        }
    }
    switch(id){
    case IDC_FTN_NET_BROWSE:ns_browse(s,0,false,s.ftn.net_file);SendMessageW(s.ftn.net_source,CB_SETCURSEL,1,0);return true;
    case IDC_FTN_NODE_BROWSE:ns_browse(s,0,false,s.ftn.node_file);return true;
    case IDC_FTN_MODE:case IDC_FTN_LISTENER:case IDC_FTN_TLS_LISTENER:ftn_listener_controls(s);return true;
    case IDC_FTN_ADD:ftn_open_network(s,true);return true;
    case IDC_FTN_SETUP:ns_begin(s);return true;
    case IDC_FTN_EDIT:ftn_open_network(s,false);return true;
    case IDC_FTN_LIST:if(notification==LBN_DBLCLK)ftn_open_network(s,false);return true;
    case IDC_FTN_REMOVE:ftn_remove_network(s);return true;
    case IDC_FTN_PRESET:if(notification==CBN_SELCHANGE&&!s.ftn.populating){const auto selected=SendMessageW(s.ftn.preset,CB_GETCURSEL,0,0);if(selected==1){door_set(s.ftn.name,"FidoNet");door_set(s.ftn.network_description,"FidoNet message network");door_set(s.ftn.url,"");}else if(selected==2){door_set(s.ftn.name,"fsxNet");door_set(s.ftn.network_description,"fsxNet message network");door_set(s.ftn.url,"https://raw.githubusercontent.com/fsxnet/infopack/master/fsxnet.na");}set_status(s,L"Editable network facts selected. Enter your assigned address and any connection details. FidoNet EchoList sources depend on your hub; no address or hub assignment is guessed.");}return true;
    case IDC_FTN_PROGRAM_BROWSE:{std::array<wchar_t,32768> buffer{};OPENFILENAMEW picker{};picker.lStructSize=sizeof(picker);picker.hwndOwner=s.window;picker.lpstrFile=buffer.data();picker.nMaxFile=static_cast<DWORD>(buffer.size());picker.lpstrFilter=L"binkd program (*.exe)\0*.exe\0\0";picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;picker.lpstrTitle=L"Choose External binkd Program";if(GetOpenFileNameW(&picker))SetWindowTextW(s.ftn.program,buffer.data());else if(CommDlgExtendedError())set_status(s,L"Program picker failed. The current external-program path remains.");SetFocus(GetDlgItem(s.pages[29],IDC_FTN_PROGRAM_BROWSE));return true;}
    case IDC_FTN_RECONCILE:{const auto result=reconcile_ftn(s);for(int i=29;i<39;++i)EnableWindow(GetDlgItem(s.pages[i],IDC_FTN_RECONCILE),s.ftn.pending.has_value());if(result.code==app::FtnConfigurationCode::ok&&result.configuration){ns_end(s);ns_show(s);s.ftn.config=result.configuration;s.ftn.draft.reset();s.ftn.draft_changed=false;s.ftn.import_base.reset();s.ftn.import_changed=false;ftn_fill(s);activate_page(s,29);set_status(s,L"The exact FTN save was confirmed. Saved networks are loaded; do not repeat the import.");}else set_status(s,s.ftn.pending?L"Save is still unconfirmed. Your draft and exact operation identity are retained.":L"No matching committed save was found. Your draft remains; review current settings before retrying.");return true;}
    case IDC_FTN_RELOAD:if(resolve_dirty(s)){refresh_ftn(s);remember_page(s);}return true;
    case IDC_FTN_SETTINGS_SAVE:case IDC_FTN_SAVE:save_ftn(s);return true;
    case IDC_FTN_LATER:save_ftn(s,true);return true;
    case IDC_FTN_BACK:if(resolve_dirty(s))activate_page(s,29);return true;
    case IDC_FTN_KEEP:EnableWindow(s.ftn.days,door_checked(s.ftn.keep));return true;
    case IDC_FTN_CONNECTIONS:ftn_open_connections(s);return true;
    case IDC_FTN_LINK_ADD:ftn_edit_link(s,true);return true;
    case IDC_FTN_LINK_EDIT:ftn_edit_link(s,false);return true;
    case IDC_FTN_LINK_LIST:if(!s.ftn.populating&&(notification==LBN_DBLCLK||notification==LBN_SELCHANGE)){const auto focused=GetFocus();ftn_edit_link(s,false);if(notification==LBN_SELCHANGE)SetFocus(focused);}return true;
    case IDC_FTN_LINK_REMOVE:ftn_remove_item(s,0);return true;
    case IDC_FTN_LINK_USE:if(notification==CBN_SELCHANGE){const bool outgoing=SendMessageW(s.ftn.link_use,CB_GETCURSEL,0,0)!=0;EnableWindow(s.ftn.link_host,outgoing);EnableWindow(s.ftn.link_port,outgoing);}return true;
    case IDC_FTN_AREAFIX_SECRET:case IDC_FTN_AREAFIX_CLEAR:case IDC_FTN_LINK_AREAFIX:ftn_areafix_state(s);return true;
    case IDC_FTN_LINK_SAVE:
        if(s.ftn.wizard){auto c=*ftn_current(s);if(collect_ftn_link(s,c)){s.ftn.draft=std::move(c);s.ftn.draft_changed=true;ftn_link_fill(s);remember_page(s);set_status(s,L"Connection retained in the setup draft. Return to Network and finish setup to save it.");}}else save_ftn(s);return true;
    case IDC_FTN_LINK_BACK:{auto c=*ftn_current(s);if(collect_ftn_link(s,c)){s.ftn.draft=std::move(c);activate_page(s,30);ftn_network_fill(s);remember_page(s);}return true;}
    case IDC_FTN_ECHOES:ftn_open_section(s,32);return true;
    case IDC_FTN_FILES:ftn_open_section(s,33);return true;
    case IDC_FTN_ROUTES:ftn_open_section(s,34);return true;
    case IDC_FTN_PERMISSIONS:s.ftn.permission_link=s.ftn.link;ftn_open_section(s,38);return true;
    case IDC_FTN_ECHO_ADD:ftn_echo_edit(s,true);return true;
    case IDC_FTN_ECHO_EDIT:ftn_echo_edit(s,false);return true;
    case IDC_FTN_ECHO_LIST:if(!s.ftn.populating&&(notification==LBN_DBLCLK||notification==LBN_SELCHANGE)){const auto focused=GetFocus();ftn_echo_edit(s,false);if(notification==LBN_SELCHANGE)SetFocus(focused);}return true;
    case IDC_FTN_ECHO_REMOVE:ftn_remove_item(s,1);return true;
    case IDC_FTN_ECHO_FIND:if(notification==EN_CHANGE&&!s.ftn.populating)ftn_echo_fill(s,true);return true;
    case IDC_FTN_ECHO_SAVE:case IDC_FTN_FILE_SAVE:case IDC_FTN_ROUTE_SAVE:save_ftn(s);return true;
    case IDC_FTN_ECHO_IMPORT:{auto c=*ftn_current(s);if(ftn_stage_current(s,c)){s.ftn.draft=std::move(c);ftn_open_import(s);}return true;}
    case IDC_FTN_FILE_ADD:ftn_file_edit(s,true);return true;
    case IDC_FTN_FILE_EDIT:ftn_file_edit(s,false);return true;
    case IDC_FTN_FILE_LIST:if(!s.ftn.populating&&(notification==LBN_DBLCLK||notification==LBN_SELCHANGE)){const auto focused=GetFocus();ftn_file_edit(s,false);if(notification==LBN_SELCHANGE)SetFocus(focused);}return true;
    case IDC_FTN_FILE_REMOVE:ftn_remove_item(s,2);return true;
    case IDC_FTN_ROUTE_ADD:ftn_route_edit(s,true);return true;
    case IDC_FTN_DEFAULT_NETMAIL_HUB:return true;
    case IDC_FTN_ROUTE_EDIT:ftn_route_edit(s,false);return true;
    case IDC_FTN_ROUTE_LIST:if(!s.ftn.populating&&(notification==LBN_DBLCLK||notification==LBN_SELCHANGE)){const auto focused=GetFocus();ftn_route_edit(s,false);if(notification==LBN_SELCHANGE)SetFocus(focused);}return true;
    case IDC_FTN_ROUTE_REMOVE:ftn_remove_item(s,3);return true;
    case IDC_FTN_PERMISSION_LINK:if(notification==CBN_SELCHANGE&&!s.ftn.populating){if(s.ftn.permission_edits){ftn_combo_select(s.ftn.permission_links,s.ftn.permission_link);set_status(s,L"Apply or save these permission choices before choosing another connection.");return true;}s.ftn.permission_link=static_cast<int>(ftn_combo_value(s.ftn.permission_links));ftn_permissions_fill(s);remember_page(s);}return true;
    case IDC_FTN_PERMISSION_APPLY:ftn_permissions_apply(s);return true;
    case IDC_FTN_PERMISSION_BACK:case IDC_FTN_ECHO_BACK:case IDC_FTN_FILE_BACK:case IDC_FTN_ROUTE_BACK:ftn_open_section(s,30);return true;
    case IDC_FTN_IMPORT_BROWSE:ftn_import_browse(s);return true;
    case IDC_FTN_IMPORT_LOAD:ftn_import_load(s);return true;
    case IDC_FTN_IMPORT_CANCEL:if(s.ftn.import_job)s.ftn.import_job->cancel=true;return true;
    case IDC_FTN_IMPORT_SEARCH:if(notification==EN_CHANGE&&!s.ftn.populating){s.ftn.import_offset=0;ftn_import_rows(s);}return true;
    case IDC_FTN_IMPORT_SELECT:case IDC_FTN_IMPORT_CLEAR:for(auto& e:s.ftn.import_list.entries)e.selected=id==IDC_FTN_IMPORT_SELECT;s.ftn.import_changed=true;ftn_import_rows(s);return true;
    case IDC_FTN_IMPORT_PREVIOUS:if(s.ftn.import_offset>=500)s.ftn.import_offset-=500;ftn_import_rows(s);return true;
    case IDC_FTN_IMPORT_NEXT:s.ftn.import_offset+=500;ftn_import_rows(s);return true;
    case IDC_FTN_IMPORT_REQUIREMENTS:ftn_import_requirements(s);return true;
    case IDC_FTN_IMPORT_APPLY:ftn_import_apply(s);return true;
    case IDC_FTN_IMPORT_RELOAD:ftn_import_reload(s);return true;
    case IDC_FTN_IMPORT_BACK:if(!ftn_import_busy(s)&&resolve_dirty(s))ftn_open_section(s,30);return true;
    case IDC_FTN_NETWORK_REVIEW:ftn_review_open(s);return true;
    case IDC_FTN_REVIEW_FINISH:save_ftn(s);return true;
    case IDC_FTN_REVIEW_BACK:ftn_open_section(s,30);return true;
    case IDC_FTN_REVIEW_ENABLE:s.ftn.enable_choice=true;return true;
    case IDC_FTN_SETUP_CANCEL:
        if(s.ftn.pending){set_status(s,L"Check Save Result before leaving this network setup.");return true;}
        if(!page_dirty(s) || ftn_confirm(s,L"Cancel Network Setup",L"Discard the unsaved network settings? Saved networks and messages are preserved.")){
            s.ftn.draft.reset();s.ftn.draft_changed=false;s.ftn.wizard=false;activate_page(s,29);
        }
        return true;
    default:return false;
    }
}
void create_ftn_pages(State& s){
    auto& f=s.ftn;auto p=s.pages[29]=page(s);
    const auto button=[&](HWND parent,const wchar_t* caption,int x,int y,int width,int id){return control(s,parent,L"BUTTON",caption,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,28,id);};
    const auto edit=[&](HWND parent,const wchar_t* caption,int x,int y,int width,int id,std::size_t maximum){label(s,parent,caption,x,y,width,20);const auto h=control(s,parent,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,x,y+22,width,25,id);SendMessageW(h,EM_SETLIMITTEXT,maximum,0);return h;};
    label(s,p,L"FTN Networks",12,8,510,25);label(s,p,L"Choose a network to edit its addresses, connections and echoes.",12,38,510,40);
    f.list=control(s,p,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,12,82,510,170,IDC_FTN_LIST);
    button(p,L"Set Up a Network...",12,264,156,IDC_FTN_SETUP);button(p,L"Add",180,264,102,IDC_FTN_ADD);button(p,L"Edit",294,264,102,IDC_FTN_EDIT);button(p,L"Remove...",408,264,114,IDC_FTN_REMOVE);
    label(s,p,L"Mailer",12,310,240,20);f.mode=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,12,332,240,140,IDC_FTN_MODE);
    for(const auto title:{L"Built-in SFX",L"External binkd",L"Disabled"})SendMessageW(f.mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    f.program=edit(p,L"External binkd program",270,310,166,IDC_FTN_PROGRAM,1024);button(p,L"Browse...",444,332,78,IDC_FTN_PROGRAM_BROWSE);
    f.listener=control(s,p,L"BUTTON",L"Accept inbound classic BinkP connections",WS_TABSTOP|BS_AUTOCHECKBOX,12,374,510,25,IDC_FTN_LISTENER);
    f.bind=edit(p,L"Shared listen address",12,410,240,IDC_FTN_BIND,253);f.port=edit(p,L"Classic port",270,410,252,IDC_FTN_PORT,5);
    f.direct=control(s,p,L"BUTTON",L"Allow direct local NetMail from unconfigured systems",WS_TABSTOP|BS_AUTOCHECKBOX,12,472,510,25,IDC_FTN_DIRECT);
    f.tls_listener=control(s,p,L"BUTTON",L"Accept inbound BinkP over TLS",WS_TABSTOP|BS_AUTOCHECKBOX,12,548,330,25,IDC_FTN_TLS_LISTENER);f.tls_port=edit(p,L"TLS port",368,526,154,IDC_FTN_TLS_PORT,5);
    f.tls_certificate=edit(p,L"TLS certificate file (required for TLS listener)",12,588,510,IDC_FTN_TLS_CERTIFICATE,1024);f.tls_key=edit(p,L"TLS private key file (required for TLS listener)",12,650,510,IDC_FTN_TLS_KEY,1024);
    button(p,L"Save Changes",12,716,240,IDC_FTN_SETTINGS_SAVE);button(p,L"Reload",270,716,252,IDC_FTN_RELOAD);label(s,p,L"External: start BinkD separately with SFXBinkD. SFX scans and imports mail while the BBS runs. Stop BinkD before backup, restore or changing mailers.",12,758,510,84);
    p=s.pages[30]=page(s);label(s,p,L"Network Settings",12,8,320,25);f.detail=label(s,p,L"A hub or first connection is optional.",12,42,330,38);f.preset=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,354,42,168,130,IDC_FTN_PRESET);for(auto title:{L"Custom Network",L"FidoNet",L"fsxNet"})SendMessageW(f.preset,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));SendMessageW(f.preset,CB_SETCURSEL,0,0);
    f.name=edit(p,L"Network name",12,92,240,IDC_FTN_NAME,96);f.address=edit(p,L"Your FTN address",270,92,252,IDC_FTN_ADDRESS,96);
    f.network_description=edit(p,L"Description (optional)",12,152,510,IDC_FTN_NETWORK_DESCRIPTION,256);
    f.aliases=edit(p,L"Additional FTN addresses (comma separated)",12,212,510,IDC_FTN_ALIASES,3200);f.origin=edit(p,L"Origin line",12,272,510,IDC_FTN_ORIGIN,128);f.url=edit(p,L"EchoList URL (HTTP or HTTPS)",12,332,510,IDC_FTN_URL,2048);
    f.enabled=control(s,p,L"BUTTON",L"Enable this network",WS_TABSTOP|BS_AUTOCHECKBOX,12,394,510,25,IDC_FTN_ENABLED);
    f.keep=control(s,p,L"BUTTON",L"Keep processed incoming originals",WS_TABSTOP|BS_AUTOCHECKBOX,12,436,510,25,IDC_FTN_KEEP);
    f.days=edit(p,L"Days to keep (0 = forever)",12,478,240,IDC_FTN_DAYS,5);
    button(p,L"Save Changes",12,550,156,IDC_FTN_SAVE);button(p,L"Save and Finish Later",180,550,180,IDC_FTN_LATER);button(p,L"Back",372,550,150,IDC_FTN_BACK);
    button(p,L"Connections...",12,598,240,IDC_FTN_CONNECTIONS);button(p,L"Message Echoes...",270,598,252,IDC_FTN_ECHOES);button(p,L"File Echoes...",12,642,240,IDC_FTN_FILES);button(p,L"Routing & Polling...",270,642,252,IDC_FTN_ROUTES);button(p,L"Review and Finish Setup...",12,686,510,IDC_FTN_NETWORK_REVIEW);
    {FtnControls ui{s,p};f.net_source=ui.combo(L"Saved echo list source",12,742,510,IDC_FTN_NET_SOURCE);for(const auto* text:{L"HTTP / HTTPS URL above",L"Local .NA file"})SendMessageW(f.net_source,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));f.net_file=ui.edit(L"Local echo list file",12,806,408,IDC_FTN_NET_FILE,1024);ui.button(L"Browse...",432,828,90,IDC_FTN_NET_BROWSE);f.node_file=ui.edit(L"Local nodelist file",12,870,408,IDC_FTN_NODE_FILE,1024);ui.button(L"Browse...",432,892,90,IDC_FTN_NODE_BROWSE);f.net_msg=ui.combo(L"External .MSG NetMail scanning for this network",12,934,510,IDC_FTN_NET_MSG);for(const auto* text:{L"Off",L"On  —  use this network's directory"})SendMessageW(f.net_msg,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));f.net_msg_path=ui.edit(L"Network .MSG NetMail directory",12,998,510,IDC_FTN_NET_MSG_PATH,1024);}
    create_ftn_connections(s);create_ftn_echoes(s);create_ftn_file_routes(s);create_ftn_import(s);create_ftn_permissions(s);create_ftn_review_held(s);ns_create(s);
    for(int i=29;i<39;++i){if(i==36)continue;const auto h=button(s.pages[i],L"Check Save Result",366,8,156,IDC_FTN_RECONCILE);EnableWindow(h,FALSE);}
}
std::optional<LRESULT> ftn_configuration_notify(State& s,const NMHDR* header){
    if(const auto handled=ns_notify(s,header))return handled;
    if(!header)return {};
    if(header->idFrom==IDC_FTN_PERMISSION_LIST&&header->code==LVN_ITEMCHANGING&&!s.ftn.populating&&s.ftn.permission_edits){const auto* row=reinterpret_cast<const NMLISTVIEW*>(header);if((row->uChanged&LVIF_STATE)&&((row->uOldState^row->uNewState)&LVIS_SELECTED)){set_status(s,L"Apply or save these permission choices before changing the selected echoes.");return TRUE;}}
    if(header->idFrom==IDC_FTN_HELD_ROWS&&header->code==LVN_ITEMCHANGED){const auto* row=reinterpret_cast<const NMLISTVIEW*>(header);if((row->uChanged&LVIF_STATE)&&((row->uOldState^row->uNewState)&LVIS_SELECTED))ftn_held_selection_changed(s);return 0;}
    if(header->idFrom==IDC_FTN_LINK_TABS&&header->code==TCN_SELCHANGE){ftn_link_tab(s);RECT r{};GetClientRect(s.window,&r);SendMessageW(s.window,WM_SIZE,0,MAKELPARAM(r.right,r.bottom));return 0;}
    if(header->idFrom==IDC_FTN_IMPORT_ROWS&&header->code==LVN_ITEMCHANGED&&!s.ftn.populating){const auto* row=reinterpret_cast<const NMLISTVIEW*>(header);if(row->iItem>=0&&static_cast<std::size_t>(row->iItem)<s.ftn.import_visible.size()&&(row->uChanged&LVIF_STATE)&&((row->uOldState^row->uNewState)&LVIS_STATEIMAGEMASK)){s.ftn.import_list.entries[s.ftn.import_visible[row->iItem]].selected=ListView_GetCheckState(s.ftn.import_rows,row->iItem)!=FALSE;s.ftn.import_changed=true;ftn_import_counts(s);}return 0;}
    if(header->idFrom==IDC_FTN_PERMISSION_LIST&&header->code==LVN_ITEMCHANGED&&!s.ftn.populating){ftn_permissions_selected(s);remember_page(s);return 0;}return {};
}
