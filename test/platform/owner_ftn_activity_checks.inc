{
    struct ReservedPort {
        SOCKET socket{INVALID_SOCKET};
        ReservedPort(){WSADATA data{};check(!WSAStartup(MAKEWORD(2,2),&data),"synthetic loopback sockets ready");socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(socket!=INVALID_SOCKET,"reserve isolated refused-connection target");BOOL exclusive=TRUE;check(!setsockopt(socket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive)),"exclusive synthetic port");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);check(!bind(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"bind only ephemeral loopback, never listen");}
        unsigned port(){sockaddr_in address{};int size=sizeof(address);check(!getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size),"read reserved loopback port");return ntohs(address.sin_port);}
        ~ReservedPort(){if(socket!=INVALID_SOCKET)closesocket(socket);WSACleanup();}
    } refused;
    const auto& original=after.value->links.front();check(!original.enabled,"activity test starts from disabled synthetic peer only");
    start();SendMessageW(control(IDC_FTN_LIST),LB_SETCURSEL,0,0);click(IDC_FTN_EDIT);if(!SendMessageW(control(IDC_FTN_ENABLED),BM_GETCHECK,0,0))click(IDC_FTN_ENABLED);click(IDC_FTN_SAVE);click(IDC_FTN_CONNECTIONS);SendMessageW(control(IDC_FTN_LINK_LIST),LB_SETCURSEL,0,0);click(IDC_FTN_LINK_EDIT);
    const auto connection_tab=[&](int index){const auto h=control(IDC_FTN_LINK_TABS);for(int step=0;step<4&&TabCtrl_GetCurSel(h)!=index;++step)SendMessageW(h,WM_KEYDOWN,TabCtrl_GetCurSel(h)<index?VK_RIGHT:VK_LEFT,0);check(TabCtrl_GetCurSel(h)==index,"actual connection subtab selected");};
    text(IDC_FTN_LINK_HOST,L"127.0.0.1");const auto port=std::to_wstring(refused.port());text(IDC_FTN_LINK_PORT,port.c_str());connection_tab(3);text(IDC_FTN_LINK_POLL,L"0");connection_tab(0);click(IDC_FTN_LINK_ENABLED);click(IDC_FTN_LINK_SAVE);
    const auto tab=GetDlgItem(panel.window,IDC_MAIN_TAB);SendMessageW(tab,WM_KEYDOWN,VK_HOME,0);SendMessageW(tab,WM_KEYDOWN,VK_RIGHT,0);check(TabCtrl_GetCurSel(tab)==1,"shipping BinkP tab selected regardless of saved tab");
    const auto poll=visible_control(panel.window,BinkPConsole::Poll),queues=visible_control(panel.window,BinkPConsole::Queues),activity=visible_control(panel.window,BinkPConsole::Activity);check(poll&&queues&&activity,"shipping BinkP actions visible");
    const auto ready_deadline=GetTickCount64()+15000;while(!IsWindowEnabled(poll)&&GetTickCount64()<ready_deadline)Sleep(30);check(IsWindowEnabled(poll),"shipping Runtime applies synthetic enabled network");
    SendMessageW(queues,BM_CLICK,0,0);check(visible_text_contains(panel.window,L"FTN QUEUES"),"actual queue view selected before Poll");
    SendMessageW(poll,BM_CLICK,0,0);check(visible_text_contains(panel.window,L"Poll requested: Owner fix regression / Synthetic peer"),"single eligible hub requests immediately and shows friendly confirmation");
    const auto failed_deadline=GetTickCount64()+30000;while(!visible_text_contains(panel.window,L"Session failed: Could not connect")&&GetTickCount64()<failed_deadline)Sleep(30);
    check(visible_text_contains(panel.window,L"Resolving hub address")&&visible_text_contains(panel.window,L"Connecting to 127.0.0.1:")&&visible_text_contains(panel.window,L"Session failed: Could not connect"),"actual loopback refusal produces visible connection phases and failure");
    check(!visible_text_contains(panel.window,utf8_to_wide(original.network))&&!visible_text_contains(panel.window,utf8_to_wide(original.code)),"shipping session and activity controls hide internal routing keys");
    render_foreign_png(panel.window,output/L"binkp-poll-activity.png");
    SendMessageW(queues,BM_CLICK,0,0);SendMessageW(activity,BM_CLICK,0,0);check(visible_text_contains(panel.window,L"Session failed: Could not connect"),"Activity Log restores actual results after queue inspection without repoll");
    #include "owner_ftn_auth_diagnostic_checks.inc"
    #include "owner_binkp_progress_checks.inc"
    text(IDC_FTN_LINK_HOST,utf8_to_wide(original.host).c_str());text(IDC_FTN_LINK_PORT,std::to_wstring(original.port).c_str());connection_tab(3);text(IDC_FTN_LINK_POLL,std::to_wstring(original.poll_minutes).c_str());connection_tab(0);click(IDC_FTN_LINK_ENABLED);click(IDC_FTN_LINK_SAVE);click(IDC_FTN_LINK_BACK);click(IDC_FTN_ENABLED);click(IDC_FTN_SAVE);close();
    const auto restored=win32::offline_ftn_configuration(root,owner);check(restored&&restored.value&&!restored.value->networks[0].enabled&&!restored.value->links[0].enabled&&restored.value->links[0].host==original.host&&restored.value->links[0].port==original.port&&restored.value->links[0].poll_minutes==original.poll_minutes&&restored.value->links[0].credentials.session==original.credentials.session,"synthetic peer settings restored through shipping Config; credentials retained");
}
