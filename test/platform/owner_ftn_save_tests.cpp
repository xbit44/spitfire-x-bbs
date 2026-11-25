// Native controls and shipping navigation handler; synthetic owner persistence.
#define SFX_FTN_UI_NO_MAIN 1
#include "ftn_ui_tests.cpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/installation_security.hpp"
LRESULT CALLBACK owner_navigation_root(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    if(m==WM_NOTIFY){const auto* n=reinterpret_cast<NMHDR*>(l);if(n&&n->idFrom==IDC_NAVIGATION){SetWindowLongPtrW(h,GWLP_USERDATA,static_cast<LONG_PTR>(data));return window_proc(h,m,w,l);}}
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,owner_navigation_root,id);
    return DefSubclassProc(h,m,w,l);
}
int main(int argc,char** argv)try{
    check(argc==2,"fresh diagnostic output required");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve previous evidence");std::filesystem::create_directories(output);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"common controls");
    ftn_query_fixture=ftn_ui_query;ftn_change_fixture=ftn_ui_change;message_query_fixture=ftn_message_query;file_query_fixture=ftn_file_query;
    ftn_model={};ftn_model.message_catalog_revision=ftn_model.file_catalog_revision=1;
    State s;s.instance=GetModuleHandleW(nullptr);s.root=output/L"synthetic-only-not-installed";s.system_recovery_only=true;s.configuration.saved.bbs_name="Owner fix fixture";
    std::filesystem::create_directories(s.root/L"Config");
    check(bool(win32::create_portable_board_key(s.root,win32::installation::operator_sid())),"current portable key for isolated password-save fixture");
    s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owner FTN save regression",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,1050,850,nullptr,nullptr,s.instance,nullptr);
    check(s.window&&SetWindowSubclass(s.window,ftn_native_root,120,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"native Config controls");
    check(SetWindowSubclass(s.window,owner_navigation_root,121,reinterpret_cast<DWORD_PTR>(&s))!=FALSE,"shipping tree navigation handler");
    requested_fonts(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);
    show_page(s,29);refresh_ftn(s);remember_page(s);
    const auto saved_tls_port=ftn_model.settings.tls_port;
    check(IsWindowEnabled(s.ftn.port)&&!IsWindowEnabled(s.ftn.tls_port)&&!IsWindowEnabled(s.ftn.tls_certificate)&&!IsWindowEnabled(s.ftn.tls_key),"inactive TLS controls disabled");
    door_set(s.ftn.port,"24777");door_set(s.ftn.tls_port,"");
    check(save_ftn(s)&&ftn_model.settings.port==24777&&!ftn_model.settings.tls_listener_enabled&&ftn_model.settings.tls_port==saved_tls_port&&!page_dirty(s),"classic24777 saves with blank inactive TLS and leaves clean page");
    const auto valid=app::encode_ftn_configuration(ftn_model);
    for(const auto bad:{"","0","65536","abc"}){door_set(s.ftn.port,bad);check(!save_ftn(s)&&page_dirty(s)&&app::encode_ftn_configuration(ftn_model)==valid&&text(s.status).find(L"classic BinkP port")!=std::wstring::npos,"invalid enabled classic port refused with precise error and edits retained");}
    discard_ftn(s);remember_page(s);door_set(s.ftn.tls_port,"bad");ftn_failure=1;
    check(!save_ftn(s)&&page_dirty(s)&&app::encode_ftn_configuration(ftn_model)==valid,"publication failure preserves inactive-field draft and saved values");ftn_failure=0;
    check(save_ftn(s)&&!page_dirty(s)&&ftn_model.settings.tls_port==saved_tls_port,"retry ignores invalid inactive TLS and preserves saved port");
    const auto valid_tls=app::encode_ftn_configuration(ftn_model);
    SendMessageW(s.ftn.tls_listener,BM_CLICK,0,0);check(IsWindowEnabled(s.ftn.tls_port)&&IsWindowEnabled(s.ftn.tls_certificate)&&IsWindowEnabled(s.ftn.tls_key),"TLS checkbox enables its settings");
    for(const auto bad:{"","0","65536","abc"}){door_set(s.ftn.tls_port,bad);check(!save_ftn(s)&&app::encode_ftn_configuration(ftn_model)==valid_tls&&text(s.status).find(L"TLS port")!=std::wstring::npos,"invalid enabled TLS port refused with precise error");}
    door_set(s.ftn.tls_port,"24778");check(!save_ftn(s)&&text(s.status).find(L"certificate and private key")!=std::wstring::npos,"enabled TLS requires certificate and key");
    door_set(s.ftn.tls_certificate,"Config/synthetic-cert.pem");door_set(s.ftn.tls_key,"Config/synthetic-key.pem");door_set(s.ftn.tls_port,"24777");
    check(!save_ftn(s)&&text(s.status).find(L"different ports")!=std::wstring::npos,"duplicate active listener ports refused clearly");
    SendMessageW(s.ftn.listener,BM_CLICK,0,0);door_set(s.ftn.port,"");
    check(!IsWindowEnabled(s.ftn.port)&&save_ftn(s)&&!ftn_model.settings.listener_enabled&&ftn_model.settings.tls_listener_enabled&&ftn_model.settings.port==24777&&ftn_model.settings.tls_port==24777,"TLS-only Save ignores inactive classic field and preserves its saved port");
    SendMessageW(s.ftn.tls_listener,BM_CLICK,0,0);door_set(s.ftn.tls_port,"");door_set(s.ftn.tls_certificate,"");door_set(s.ftn.tls_key,"");
    check(save_ftn(s)&&!ftn_model.settings.tls_listener_enabled&&ftn_model.settings.tls_certificate=="Config/synthetic-cert.pem"&&ftn_model.settings.tls_private_key=="Config/synthetic-key.pem"&&ftn_model.settings.tls_port==24777&&!page_dirty(s),"both listeners disabled saves without erasing retained TLS settings");
    SendMessageW(s.ftn.listener,BM_CLICK,0,0);door_set(s.ftn.port,"24777");door_set(s.ftn.tls_port,"");
    std::atomic<bool> defaults_answered{};std::thread defaults_dialog([&]{const auto until=GetTickCount64()+7000;while(GetTickCount64()<until){const auto h=FindWindowW(L"#32770",kWindowTitle);DWORD pid{};if(h)GetWindowThreadProcessId(h,&pid);if(h&&pid==GetCurrentProcessId()&&GetWindow(h,GW_OWNER)==s.window&&IsWindowVisible(h)){PostMessageW(h,TDM_CLICK_BUTTON,101,0);defaults_answered=true;return;}Sleep(10);}});
    const auto defaults_navigated=TreeView_SelectItem(s.navigation,s.route_items[0]);defaults_dialog.join();
    check(defaults_answered&&defaults_navigated&&s.current_page==0&&ftn_model.settings.listener_enabled&&ftn_model.settings.port==24777&&!page_dirty(s),"Networks Save and Continue saves24777 then leaves without loop");
    show_page(s,29);refresh_ftn(s);remember_page(s);check(door_text(s.ftn.port)=="24777"&&!door_checked(s.ftn.tls_listener),"reopened defaults retain24777 and TLS off");
    ftn_open_network(s,true,false);
    door_set(s.ftn.name,"Owner regression network");door_set(s.ftn.address,"42:100/1@own-test");check(save_ftn(s),"save network");
    ftn_open_connections(s);check(!page_dirty(s),"opening Connections does not manufacture unsaved edits");
    ftn_edit_link(s,true);door_set(s.ftn.link_name,"Synthetic peer");door_set(s.ftn.link_address,"42:100/2@own-test");check(save_ftn(s),"save disabled connection");
    SendMessageW(s.ftn.links,LB_SETCURSEL,0,0);ftn_configuration_command(s,IDC_FTN_LINK_LIST,LBN_SELCHANGE);check(!page_dirty(s),"selecting saved connection is not an edit");
    const auto node=[&](int page){for(const auto& n:s.ftn.navigation)if(n.page==page&&n.network==ftn_model.networks[0].code&&TreeView_GetParent(s.navigation,n.handle)!=s.route_items[29])return n.handle;return static_cast<HTREEITEM>(nullptr);};
    const auto connection=node(31),echoes=node(32);check(connection&&echoes,"stable named navigation targets");
    check(TreeView_SelectItem(s.navigation,connection)!=FALSE,"real tree enters Connections");
    SendMessageW(s.ftn.links,LB_SETCURSEL,0,0);ftn_configuration_command(s,IDC_FTN_LINK_LIST,LBN_SELCHANGE);
    TabCtrl_SetCurSel(s.ftn.link_tabs,1);ftn_link_tab(s);
    for(std::size_t i=0;i<4;++i)door_set(s.ftn.secrets[i],i==1?"PKTtest":"SyntheticSecretOnly");
    check(page_dirty(s),"password edits dirty before Save");
    check(save_ftn(s)&&!page_dirty(s),"password Save leaves clean page");
    check(node(31)==connection&&node(32)==echoes,"save preserves actual tree handles");
    for(std::size_t i=0;i<4;++i)check(text(s.ftn.secrets[i])==(i==1?L"PKTtest":L"SyntheticSecretOnly")&&!SendMessageW(s.ftn.secrets[i],EM_GETPASSWORDCHAR,0,0),"saved password shown as readable editable text");
    const auto unchanged=app::encode_ftn_configuration(ftn_model);auto same=ftn_model;check(collect_ftn_link(s,same)&&app::encode_ftn_configuration(same)==unchanged,"viewing passwords does not reseal or change stored values");
    door_set(s.ftn.secrets[3],"ChangedTic");ftn_failure=1;check(!save_ftn(s)&&text(s.ftn.secrets[3])==L"ChangedTic"&&page_dirty(s),"failed password Save preserves visible edits");ftn_failure=0;discard_ftn(s);remember_page(s);check(text(s.ftn.secrets[3])==L"SyntheticSecretOnly"&&!page_dirty(s),"Discard restores saved visible password");
    door_set(s.ftn.secrets[3],"");check(save_ftn(s)&&ftn_model.links[0].credentials.tic.empty()&&text(s.ftn.secrets[3]).empty(),"clearing visible password removes it on Save");door_set(s.ftn.secrets[3],"SyntheticSecretOnly");check(save_ftn(s),"restore synthetic TIC password");
    const auto& secret=ftn_model.links[0].credentials;check(!secret.session.empty()&&!secret.packet.empty()&&!secret.areafix.empty()&&!secret.tic.empty(),"all four protected credentials reached publisher");
    // Checkbox-only edits must survive leaving their subtab.
    SendMessageW(s.ftn.clear[0],BM_CLICK,0,0);TabCtrl_SetCurSel(s.ftn.link_tabs,3);ftn_link_tab(s);check(page_dirty(s),"hidden remove-password checkbox stays dirty");
    check(save_ftn(s)&&ftn_model.links[0].credentials.session.empty()&&!page_dirty(s),"hidden checkbox saved and clean");
    // Exercise the exact TaskDialog Save and Continue path and pending tree node.
    door_set(s.ftn.link_poll,"17");
    std::atomic<bool> answered{};std::thread dialog([&]{const auto until=GetTickCount64()+7000;while(GetTickCount64()<until){const auto h=FindWindowW(L"#32770",kWindowTitle);DWORD pid{};if(h)GetWindowThreadProcessId(h,&pid);if(h&&pid==GetCurrentProcessId()&&GetWindow(h,GW_OWNER)==s.window&&IsWindowVisible(h)){PostMessageW(h,TDM_CLICK_BUTTON,101,0);answered=true;return;}Sleep(10);}});
    const auto navigated=TreeView_SelectItem(s.navigation,echoes);dialog.join();
    check(answered&&navigated&&s.current_page==32&&ftn_model.links[0].poll_minutes==17&&!page_dirty(s),"actual Save and Continue commits then navigates without loop");
    check(TreeView_SelectItem(s.navigation,connection)!=FALSE,"return through real tree after saved navigation");SendMessageW(s.ftn.links,LB_SETCURSEL,0,0);ftn_configuration_command(s,IDC_FTN_LINK_LIST,LBN_SELCHANGE);check(!page_dirty(s),"reopen connection stays clean");
    // Permissions Save must apply the current choices rather than discard them.
    app::FtnMessageEcho echo;echo.network=ftn_model.networks[0].code;echo.tag="TEST.ECHO";ftn_model.message_echoes.push_back(echo);++ftn_model.revision;
    refresh_ftn(s);s.ftn.network=0;s.ftn.permission_link=0;s.ftn.draft=*s.ftn.config;activate_page(s,38);
    ListView_SetItemState(s.ftn.permission_rows,0,LVIS_SELECTED,LVIS_SELECTED);SendMessageW(s.ftn.permission_receive,BM_CLICK,0,0);
    check(s.ftn.permission_edits&&page_dirty(s),"permission choice is pending");
    check(save_ftn(s)&&ftn_model.permissions.size()==1&&ftn_model.permissions[0].receive&&!page_dirty(s),"Save applies selected rights without separate Apply");
    // Failed publication keeps staged rights and permits a truthful retry.
    ListView_SetItemState(s.ftn.permission_rows,0,LVIS_SELECTED,LVIS_SELECTED);SendMessageW(s.ftn.permission_send,BM_CLICK,0,0);ftn_failure=1;
    check(!save_ftn(s)&&page_dirty(s)&&!ftn_model.permissions[0].send,"failed Save retains unsaved rights");ftn_failure=0;check(save_ftn(s)&&ftn_model.permissions[0].send,"retry commits retained rights");
    layout(s,1050,850);render_png(s,output/L"permission-save-complete.png");

    // Only selected-network scanning is editable. Old shared settings are retained but inert.
    check(sfx::config_ui::route(L"ftn.msg_netmail").id==L"ftn.networks"&&!s.pages[39],"retired shared route falls back to Networks with no editor");
    s.ftn.network=0;s.ftn.draft=*s.ftn.config;activate_page(s,30);remember_page(s);
    check(SendMessageW(s.ftn.net_msg,CB_GETCOUNT,0,0)==2&&SendMessageW(s.ftn.net_msg,CB_GETCURSEL,0,0)==0,"legacy unset choice opens Off with exactly two choices");
    SendMessageW(s.ftn.net_msg,CB_SETCURSEL,1,0);check(page_dirty(s)&&!save_ftn(s),"network On requires own directory and keeps draft");
    door_set(s.ftn.net_msg_path,"C:\\RC8 Synthetic\\NetMail");check(save_ftn(s)&&!page_dirty(s)&&ftn_model.networks[0].msg_netmail_enabled==true,"explicit network On persists");
    const auto saved_msg=ftn_model.networks[0].msg_netmail_directory;activate_page(s,29);s.ftn.network=0;s.ftn.draft=*s.ftn.config;activate_page(s,30);
    check(door_text(s.ftn.net_msg_path)==saved_msg&&SendMessageW(s.ftn.net_msg,CB_GETCURSEL,0,0)==1&&!page_dirty(s),"per-network directory and enable survive page return");
    door_set(s.ftn.net_msg_path,"C:\\RC8 Uncommitted");ftn_failure=1;check(!save_ftn(s)&&page_dirty(s)&&ftn_model.networks[0].msg_netmail_directory==saved_msg,"failed network save preserves persisted path and draft");ftn_failure=0;discard_ftn(s);remember_page(s);
    check(s.current_page==30&&!page_dirty(s)&&door_text(s.ftn.net_msg_path)==saved_msg,"Discard restores selected network directory");
    SendMessageW(s.ftn.net_msg,CB_SETCURSEL,0,0);check(save_ftn(s)&&ftn_model.networks[0].msg_netmail_enabled==false&&ftn_model.networks[0].msg_netmail_directory==saved_msg,"network Off retains configured directory");
    layout(s,1050,850);render_png(s,output/L"network-msg-netmail-saved.png");

    app::FtnLink hub;
    hub.network=ftn_model.networks[0].code;
    hub.code="default-hub";hub.name="Synthetic hub";
    hub.address="42:100/81@own-test";hub.host="127.0.0.1";hub.enabled=true;
    const auto hub_choice=static_cast<std::int64_t>(ftn_model.links.size()+1);
    ftn_model.links.push_back(hub);
    auto inbound=hub;
    inbound.code="inbound-only";inbound.name="Inbound only";
    inbound.address="42:100/82@own-test";inbound.use=app::FtnConnectionUse::inbound_only;
    ftn_model.links.push_back(inbound);
    ++ftn_model.revision;
    refresh_ftn(s);
    const auto routing=node(34);
    check(routing&&TreeView_SelectItem(s.navigation,routing)&&s.current_page==34,"open the existing network's Routing & Polling page");
    const auto select_hub=[&](std::int64_t choice){
        ftn_combo_select(s.ftn.default_netmail_hub,choice);
        ftn_configuration_command(s,IDC_FTN_DEFAULT_NETMAIL_HUB,CBN_SELCHANGE);
    };
    check(SendMessageW(s.ftn.default_netmail_hub,CB_GETCOUNT,0,0)==2&&ftn_combo_value(s.ftn.default_netmail_hub)==0,
        "the hub picker starts at None and excludes disabled and inbound-only links");
    select_hub(hub_choice);ftn_failure=1;
    check(page_dirty(s)&&!save_ftn(s)&&ftn_model.networks[0].default_link.empty()&&
        ftn_combo_value(s.ftn.default_netmail_hub)==hub_choice,"failed hub save keeps the selected draft");
    ftn_failure=0;
    check(save_ftn(s)&&!page_dirty(s)&&ftn_model.networks[0].default_link==hub.code&&ftn_model.routes.empty(),
        "save the default hub without creating a specific route");
    check(TreeView_SelectItem(s.navigation,echoes)&&TreeView_SelectItem(s.navigation,routing)&&
        !page_dirty(s)&&ftn_combo_value(s.ftn.default_netmail_hub)==hub_choice,"the saved hub is shown when Routing & Polling reopens");
    select_hub(0);discard_ftn(s);remember_page(s);
    check(!page_dirty(s)&&ftn_combo_value(s.ftn.default_netmail_hub)==hub_choice,"Discard restores the saved hub");
    select_hub(0);
    check(save_ftn(s)&&!page_dirty(s)&&ftn_model.networks[0].default_link.empty(),"None clears the saved default hub");
    DestroyWindow(s.window);std::cout<<"PASS "<<checks<<" owner FTN save/navigation checks; synthetic publisher, actual native controls/tree/dialog\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
