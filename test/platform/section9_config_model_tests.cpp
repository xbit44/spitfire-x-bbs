// Hidden, in-process native-control/model tests with synthetic ports only.
// No real installation/account/security change, dialogs, screen capture, UI
// automation of another app, or manual/installed-UI acceptance is claimed.
#define SFX_ALPHA_TEST_AUTOMATION 1
#define wWinMain retained_section9_config_entry
#include "../../src/apps/config_ui/main.cpp"
#undef wWinMain
#include <iostream>
namespace {
unsigned checks{},changes{};bool unavailable{},conflict{},lost_response{};
app::FileConfiguration catalog;
app::FileCallerExemption exempt{42,1,false,false};
app::FileConfigurationChange last_change;
app::FileChangeOutcome last_outcome;
void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
app::LocalFileConfigurationSnapshot snapshot(const app::LocalFileConfigurationQuery& q){
    if(unavailable)return {};app::LocalFileConfigurationSnapshot s;s.code=app::LocalAccountResultCode::ok;s.part=q.part;s.policy=catalog.policy;
    if(q.catalog_revision&&q.catalog_revision!=s.policy.catalog_revision)return {app::LocalAccountResultCode::conflict};
    if(q.part==app::FileConfigurationPart::caller_exemption){s.exemption=exempt;s.exemption->account=q.account;return s;}
    const auto page=[&](const auto& source,auto& output,const auto& id,const auto& included){for(const auto& item:source)if(id(item)>q.after_id&&included(item)){if(output.size()==12){s.next_after=id(output.back());break;}output.push_back(item);}};
    if(q.part==app::FileConfigurationPart::groups)page(catalog.groups,s.groups,[](const auto& g){return g.id.value;},[](const auto&){return true;});
    if(q.part==app::FileConfigurationPart::areas)page(catalog.areas,s.areas,[](const auto& a){return a.area.id.value;},[&](const auto& a){return !q.group.value||a.area.group==q.group;});
    if(q.part==app::FileConfigurationPart::security_rules)page(catalog.security_rules,s.security_rules,[](const auto& r){return r.level;},[](const auto&){return true;});
    return s;
}
app::LocalFileConfigurationResult change(const app::FileConfigurationChange& c){
    ++changes;if(last_change.operation.valid()&&last_change.operation==c.operation)return {app::LocalAccountResultCode::ok,{last_outcome.revision,last_outcome.identity,true}};
    if(conflict||c.expected_catalog_revision!=catalog.policy.catalog_revision)return {app::LocalAccountResultCode::conflict};
    check(app::valid_file_configuration_change(c),"UI submits a fully valid typed candidate");last_change=c;last_outcome={++catalog.policy.catalog_revision,0,false};
    switch(c.action){
    case app::FileConfigurationAction::policy:{const auto revision=catalog.policy.revision+1;catalog.policy=c.policy;catalog.policy.revision=revision;catalog.policy.catalog_revision=last_outcome.revision;break;}
    case app::FileConfigurationAction::group:{const auto g=std::find_if(catalog.groups.begin(),catalog.groups.end(),[&](const auto& v){return v.id==c.group.id;});if(g!=catalog.groups.end()){*g=c.group;++g->revision;}else{auto value=c.group;value.id={static_cast<std::int64_t>(catalog.groups.size()+1)};catalog.groups.push_back(value);}break;}
    case app::FileConfigurationAction::area:{last_outcome.identity=c.area.area.id.value;const auto a=std::find_if(catalog.areas.begin(),catalog.areas.end(),[&](const auto& v){return v.area.id==c.area.area.id;});if(a!=catalog.areas.end()){*a=c.area;++a->area.revision;}break;}
    case app::FileConfigurationAction::security_rule:catalog.security_rules.clear();if(!c.security.inherit){auto rule=c.security;++rule.revision;catalog.security_rules.push_back(rule);}break;
    case app::FileConfigurationAction::caller_exemption:exempt=c.exemption;++exempt.revision;break;
    }
    if(lost_response){lost_response=false;return {};}
    return {app::LocalAccountResultCode::ok,last_outcome};
}
}
int main(int argc,char** argv){State s;try{
    check(argc==2||argc==3,"DPI and optional fresh offline log fixture arguments required");s.dpi=std::stoi(argv[1]);check(s.dpi==96||s.dpi==144,"bounded supported test DPI");s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\runs\\section9-config-synthetic-not-installed";
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native control classes initialized");
    s.window=CreateWindowExW(0,L"STATIC",L"Section9 hidden model fixture",WS_OVERLAPPEDWINDOW,0,0,980,760,nullptr,nullptr,s.instance,nullptr);check(s.window&&create_ui(s),"current native controls created without an installed BBS");
    check(SendMessageW(s.menu_select,CB_GETCOUNT,0,0)==12,"preview offers exactly twelve current menus");
    check(SendMessageW(s.menu_select,CB_GETITEMDATA,10,0)==static_cast<LRESULT>(app::MenuId::batch_transfer)&&
        SendMessageW(s.menu_select,CB_GETITEMDATA,11,0)==static_cast<LRESULT>(app::MenuId::qwk),"preview maps combined Batch and QWK to stable menu IDs");
    for(unsigned item=0;item<12;++item){const auto menu=SendMessageW(s.menu_select,CB_GETITEMDATA,item,0);check(menu>=0&&menu!=3&&menu!=4,"retired preview entries absent");}
    check(s.pages.size()==42&&sfx::config_ui::route(L"files.groups_areas").page==21&&sfx::config_ui::route(L"files.caller_exemptions").page==24,"four stable functional file routes and appended ClamAV page");
    check(TreeView_GetCount(s.navigation)>=31,"only functional tree entries added");
    check(text(GetDlgItem(s.pages[21],IDC_FILE_MANAGER))==L"Open File Manager","discoverable File Manager launcher on Files page");
    for(unsigned n=1;n<=25;++n)catalog.groups.push_back({{n},"Group "+std::to_string(n),static_cast<std::int64_t>(n-1),1,1,true});
    app::FileAreaConfiguration ordinary;ordinary.area.id={1};ordinary.area.group={1};ordinary.area.number=1;ordinary.area.name="General Files";ordinary.storage_path="Files/General";
    auto private_area=ordinary;private_area.area.id={2};private_area.area.number=2;private_area.area.name="Upload to Sysop";private_area.area.kind=app::FileAreaKind::private_sysop;private_area.area.permissions.downloads_sysop_only=true;private_area.storage_path="Files/UploadToSysop";catalog.areas={ordinary,private_area};
    file_query_fixture=snapshot;file_change_fixture=change;
    account_query_fixture=[](const app::LocalAccountQuery& q){app::LocalAccountSnapshot v;v.code=app::LocalAccountResultCode::ok;for(const auto id:{42,43})if(!q.account_id||q.account_id==id){app::AccountRecord a;a.account_id=id;a.display_name=id==42?"Alice":"Bob";v.accounts.push_back(a);}return v;};
    file_levels_fixture=[](){app::LocalSecuritySnapshot v;v.code=app::LocalAccountResultCode::ok;app::SecurityLevelRecord level;level.level=10;level.label="Caller";v.levels.push_back(level);return v;};
    auto& f=s.file_editor;show_page(s,21);refresh_file_configuration_page(s);
    check(f.ready&&f.groups.size()==25&&SendMessageW(f.parent,CB_GETCOUNT,0,0)==25,"complete bounded IPC pages assembled for group selector and parent choices");
    check(f.group&&!f.area&&!IsWindowEnabled(f.path)&&!page_dirty(s),"group editing cannot silently edit an area path");
    check(text(f.name_label)==L"Group Name","selected Group clearly labeled");
    file_configuration_command(s,IDC_FILE_NEW_AREA,BN_CLICKED);check(f.area&&!f.group&&text(f.name_label)==L"Area Name"&&text(f.name).empty()&&IsWindowEnabled(f.upload),"New Area visibly switches editor and area-only controls");
    refresh_file_configuration_page(s);

    SendMessageW(f.area_list,LB_SETCURSEL,1,0);file_configuration_command(s,IDC_FILE_AREAS,LBN_SELCHANGE);
    check(f.area&&f.area->area.kind==app::FileAreaKind::private_sysop&&!IsWindowEnabled(f.sysop_downloads)&&Button_GetCheck(f.sysop_downloads)==BST_CHECKED,"permanent private-area download role remains read-only");
    check(!Button_GetCheck(f.backup_contents),"area content backup default remains Off");
    check(file_configuration_path_spelling(L"C:\\SFX-Test","Files/General",L"c:/sfx-test/Files/General")==L"Files/General","picker equivalent absolute spelling keeps saved relative path");
    check(file_configuration_path_spelling(L"C:\\SFX-Test","Files/General",L"C:\\different")==L"C:\\different","different explicit folder is not silently redirected");
    check(file_configuration_path_spelling(L"C:\\SFX-Test","Files/General",L"C:\\SFX-Test\\Files\\x\\..\\General")==L"C:\\SFX-Test\\Files\\x\\..\\General","invalid traversal is not normalized into an accepted saved path");
    SetWindowTextW(f.path,(s.root/L"Files"/L"UploadToSysop").c_str());
    SetWindowTextW(f.name,L"Private submissions");check(page_dirty(s)&&save_file_configuration_page(s)&&last_change.area.area.name=="Private submissions"&&last_change.area.storage_path=="Files/UploadToSysop","only selected area metadata is submitted without path relocation");
    check(text(f.name_label)==L"Area Name","saved selected Area label");
    catalog.areas[0].area.order=10;catalog.areas[1].area.order=0;refresh_file_configuration_page(s);check(f.areas[0].area.id.value==2&&f.areas[1].area.id.value==1,"Config display order independent of IPC identity pages");
    SendMessageW(f.area_list,LB_SETCURSEL,1,0);file_configuration_command(s,IDC_FILE_AREAS,LBN_SELCHANGE);const auto anchor=f.area->area.id;file_prepare_insert(s,true);check(f.insert_anchor==anchor&&f.insert_after&&f.area&&!f.area->area.id.value&&text(f.name_label)==L"Area Name"&&!IsWindowEnabled(f.parent),"Insert After prepares visible new Area under saved anchor");
    catalog.areas[0].area.order=0;catalog.areas[1].area.order=0;refresh_file_configuration_page(s);
    show_page(s,22);refresh_file_configuration_page(s);
    check(f.ready&&text(f.policy_cap)==L"104857600"&&text(f.ratio_down).empty()&&text(f.ratio_up).empty()&&!IsWindowEnabled(f.ratio_down),"fresh global defaults show bytes, ratio Off and no prefilled ratio");
    auto before=changes;Button_SetCheck(f.ratio_on,BST_CHECKED);file_count_controls(s);check(!save_file_configuration_page(s)&&changes==before,"enabling ratio without explicit values refuses before submission");
    SetWindowTextW(f.ratio_down,L"5");SetWindowTextW(f.ratio_up,L"1");Button_SetCheck(f.daily_on,BST_CHECKED);SetWindowTextW(f.daily,L"12");SetWindowTextW(f.policy_cap,L"33554432");
    check(save_file_configuration_page(s)&&catalog.policy.counts.ratio->downloads==5&&catalog.policy.counts.daily_downloads==12U&&catalog.policy.maximum_upload_bytes==33554432,"explicit global count policy and byte maximum saved together");
    before=changes;SetWindowTextW(f.policy_cap,L"0");check(!save_file_configuration_page(s)&&changes==before&&text(f.policy_cap)==L"0","invalid maximum retains the draft without any request");SetWindowTextW(f.policy_cap,L"33554432");
    lost_response=true;SetWindowTextW(f.policy_cap,L"16777216");check(!save_file_configuration_page(s)&&f.pending&&!IsWindowEnabled(f.policy_cap)&&IsWindowEnabled(GetDlgItem(s.pages[22],IDC_FILE_RECONCILE)),"uncertain save freezes edits and offers exact reconcile");
    const auto operation=f.pending->operation;file_configuration_command(s,IDC_FILE_RECONCILE,BN_CLICKED);check(!f.pending&&f.ready&&last_change.operation==operation&&text(f.policy_cap)==L"16777216"&&!page_dirty(s),"same operation reconciled without fabricating a second mutation");
    show_page(s,23);refresh_file_configuration_page(s);check(f.ready&&f.rule.level==10&&Button_GetCheck(f.rule_inherit)==BST_CHECKED&&!IsWindowEnabled(f.rule_ratio_down),"existing configured level starts with explicit inheritance");
    Button_SetCheck(f.rule_inherit,BST_UNCHECKED);Button_SetCheck(f.rule_ratio_on,BST_CHECKED);SetWindowTextW(f.rule_ratio_down,L"3");SetWindowTextW(f.rule_ratio_up,L"2");
    check(save_file_configuration_page(s)&&catalog.security_rules.size()==1&&catalog.security_rules[0].counts.ratio->uploads==2,"selected level override maps to correct typed rule only");
    Button_SetCheck(f.rule_inherit,BST_CHECKED);check(save_file_configuration_page(s)&&catalog.security_rules.empty(),"restoring inheritance does not create an invented explicit ratio");
    show_page(s,24);refresh_file_configuration_page(s);check(f.ready&&f.accounts.accounts.size()==2&&!f.exemption&&!IsWindowEnabled(f.exempt_daily),"exemptions require an exact selected account snapshot");
    SendMessageW(f.exempt_select,CB_SETCURSEL,0,0);file_configuration_command(s,IDC_FILE_EXEMPT_SELECT,CBN_SELCHANGE);Button_SetCheck(f.exempt_daily,BST_CHECKED);
    check(save_file_configuration_page(s)&&exempt.account==42&&exempt.daily_exempt&&!exempt.ratio_exempt,"selected caller daily exemption does not change ratio exemption or another caller");
    show_page(s,22);refresh_file_configuration_page(s);conflict=true;SetWindowTextW(f.policy_cap,L"8388608");check(!save_file_configuration_page(s)&&!f.pending&&page_dirty(s)&&text(f.policy_cap)==L"8388608","conflict preserves editable draft without claiming applied success");conflict=false;discard_page(s);remember_page(s);
    unavailable=true;refresh_file_configuration_page(s);before=changes;check(!f.ready&&!save_file_configuration_page(s)&&changes==before&&IsWindowEnabled(GetDlgItem(s.pages[22],IDC_FILE_POLICY_RELOAD)),"unavailable snapshot permits Reload but no stale draft submission");unavailable=false;refresh_file_configuration_page(s);
    check(sfx::config_ui::route(L"security.clamav").page==25&&sfx::config_ui::security_page(25)&&TabCtrl_GetItemCount(s.security_tabs)==4,"ClamAV is the fourth stable native security tab, not a duplicate tree editor");
    check(GetParent(f.scan)==s.pages[25]&&!GetDlgItem(s.pages[22],IDC_FILE_SCAN),"scan checkbox has exactly one home, no editable Transfer Policy copy");
    s.mode=State::ConnectionMode::offline;show_page(s,25);refresh_file_configuration_page(s);s.clam_status_read=State::ClamStatusRead::ready;s.clam_status.diagnostic="clamav_not_installed";render_clam(s);
    check(text(s.clam_detail).find(L"Not installed")!=std::wstring::npos&&text(s.clam_detail).find(L"Unavailable")!=std::wstring::npos&&IsWindowEnabled(s.clam_install)&&!IsWindowEnabled(s.clam_update),"missing engine/history is truthful and permits only trusted install-later");
    const auto old_cap=catalog.policy.maximum_upload_bytes;const auto old_daily=catalog.policy.counts.daily_downloads;const auto old_ratio=catalog.policy.counts.ratio;Button_SetCheck(f.scan,BST_CHECKED);
    check(page_dirty(s)&&save_current_page(s)&&catalog.policy.scan_uploads&&catalog.policy.maximum_upload_bytes==old_cap&&catalog.policy.counts.daily_downloads==old_daily&&catalog.policy.counts.ratio->downloads==old_ratio->downloads&&!page_dirty(s),"ClamAV Save Changes edits only revisioned scanning policy and remembers saved draft");
    Button_SetCheck(f.scan,BST_UNCHECKED);lost_response=true;check(!save_file_configuration_page(s)&&f.pending&&!IsWindowEnabled(f.scan)&&!IsWindowEnabled(s.clam_save),"uncertain ClamAV policy save freezes the single editor");
    file_configuration_command(s,IDC_FILE_RECONCILE,BN_CLICKED);check(!f.pending&&!catalog.policy.scan_uploads&&!page_dirty(s),"ClamAV uses existing exact-operation reconcile, no second policy owner");
    Button_SetCheck(f.scan,BST_CHECKED);show_page(s,22);refresh_file_configuration_page(s);SetWindowTextW(f.policy_cap,L"8388608");check(save_file_configuration_page(s)&&!catalog.policy.scan_uploads,"Transfer Policy never resaves a stale scan checkbox from another page");
    show_page(s,25);refresh_file_configuration_page(s);s.clam_status_read=State::ClamStatusRead::ready;s.clam_status.installed=true;s.clam_status.verified=true;s.clam_status.version="1.4.6";s.clam_status.definitions_present=true;render_clam(s);
    check(text(s.clam_detail).find(L"readiness not verified")!=std::wstring::npos&&!IsWindowEnabled(s.clam_install)&&IsWindowEnabled(s.clam_update),"legacy definitions present does not fabricate verified readiness");
    s.clam_status.history_available=true;s.clam_status.last_attempt=1792316700;s.clam_status.last_success=1792316600;s.clam_status.last_mode=win32::ClamAvUpdateMode::manual;s.clam_status.last_result="clamav_update_vendor_cooldown";s.clam_status.definitions_verified=true;s.mode=State::ConnectionMode::online;render_clam(s);
    check(text(s.clam_detail).find(L"UTC")!=std::wstring::npos&&text(s.clam_detail).find(L"(manual)")!=std::wstring::npos&&text(s.clam_detail).find(L"vendor_cooldown")!=std::wstring::npos&&text(s.clam_detail).find(L"Runtime is running")!=std::wstring::npos,"verified history, manual failure and separate Runtime automatic status shown without inventing update success");
    s.clam_status.last_result="clamav_definitions_verified";s.clam_status.change=win32::ClamAvDefinitionChange::current;s.clam_status.databases[1]=win32::ClamAvDatabaseVersion{123,456,1792316600};render_clam(s);
    check(text(s.clam_detail).find(L"Already current")!=std::wstring::npos&&text(s.clam_detail).find(L"Daily: version 123; published")!=std::wstring::npos&&text(s.clam_detail).find(L"Last definition change: Unavailable")!=std::wstring::npos,"verified metadata and current outcome without fabricated legacy change time");
    check(s.clam_log&&text(s.clam_log)==L"View Update Log"&&IsWindowEnabled(s.clam_log),"read-only log action remains available during normal online status");
    {
        const wchar_t* name=L"SFXClamAVReadOnlyUpdateLog";WNDCLASSW cls{};cls.hInstance=s.instance;cls.lpfnWndProc=clam_log_proc;cls.lpszClassName=name;check(RegisterClassW(&cls)!=0,"isolated native update viewer class");
        ClamLogWindow view;view.root=s.root;view.owner=s.owner;
        const auto window=CreateWindowW(name,L"Hidden native viewer test",WS_OVERLAPPEDWINDOW,0,0,640,480,s.window,nullptr,s.instance,&view);
        check(window&&view.edit&&view.refresh&&(GetWindowLongPtrW(view.edit,GWL_STYLE)&ES_READONLY)&&!(GetWindowLongPtrW(view.edit,GWL_STYLE)&ES_PASSWORD),"actual hidden viewer has scrollable read-only native edit, no secret-field reuse");
        SendMessageW(window,WM_COMMAND,1,0);check(text(view.edit).find(L"unavailable")!=std::wstring::npos,"Refresh handles unavailable log without updater or success claim");DestroyWindow(window);
    }
    if(argc==3){
        const auto root=std::filesystem::absolute(argv[2]);check(root.parent_path()==std::filesystem::path(L"C:\\sfxtst\\runs")&&root.filename().string().rfind("section9-clamav-visibility-offline-",0)==0&&!std::filesystem::exists(root),"fresh bounded offline attention fixture only");
        const auto owner=install::operator_sid();win32::WindowsStorageDaclPolicy private_policy(owner);for(const auto& folder:{root,root/L"Logs"}){std::filesystem::create_directory(folder);check(private_policy.apply(folder,win32::StorageDaclObjectKind::directory).succeeded(),"private offline journal fixture");}
        app::ClamAvAttentionTracker health;const auto failed=health.observe(false,false,true,"clamav_update_vendor_cooldown",1792316600);const app::LoggingConfiguration limits;
        check(failed&&offline_clam_attention(root,owner,limits,"UTC",*failed),"stopped-BBS failure uses protected existing journal path and BBS timezone");
        check(!health.observe(false,false,true,"clamav_update_vendor_cooldown",1792316601),"stopped-BBS repeated failure produces no duplicate notice");
        const auto recovered=health.observe(false,true,true,{},1792316602);check(recovered&&offline_clam_attention(root,owner,limits,"UTC",*recovered),"stopped-BBS actual health transition records recovery");
        win32::WindowsDurableJournalFiles journals(root,owner);const auto state=journals.inspect(app::DurableJournalKind::attention);const auto raw=state?journals.read(app::DurableJournalKind::attention,0,static_cast<std::size_t>(state->bytes)):std::nullopt;
        check(raw&&raw->find("CLAMAV_FAILURE")!=std::string::npos&&raw->find("CLAMAV_RECOVERY")!=std::string::npos&&raw->find("validated definitions remain usable")!=std::string::npos,"actual stopped-BBS HEYSYSOP bytes preserve failure/recovery and usable-definition distinction");
    }
    show_page(s,21);refresh_file_configuration_page(s);layout(s,MulDiv(980,s.dpi,96),MulDiv(620,s.dpi,96));SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE};GetScrollInfo(s.pages[21],SB_VERT,&scroll);check(scroll.nMax>=static_cast<int>(scroll.nPage),"long functional area page uses vertical scrolling at selected DPI");
    file_query_fixture=nullptr;file_change_fixture=nullptr;file_levels_fixture=nullptr;account_query_fixture=nullptr;DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);
    std::cout<<"PASS file Config model "<<checks<<" checks at DPI"<<s.dpi<<"; hidden owned controls/synthetic ports only, not installed GUI or manual acceptance\n";return 0;
}catch(const std::exception& e){if(s.window)DestroyWindow(s.window);std::cerr<<"FAIL file Config model after "<<checks<<": "<<e.what()<<'\n';return 1;}}
