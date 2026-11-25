// Persistent configuration only. Payload/catalog maintenance belongs to the
// separate File Manager. All requests use the protected owner relay or an
// exclusive stopped-installation session; no GUI SQL or success assertions.
bool resolve_dirty(State&);
void refresh_file_configuration_page(State&);
void file_configuration_editability(State&);
void file_catalog_fields(State&);
bool file_load_areas(State&,app::FileGroupId);
#ifdef SFX_ALPHA_TEST_AUTOMATION
app::LocalFileConfigurationSnapshot (*file_query_fixture)(const app::LocalFileConfigurationQuery&)=nullptr;
app::LocalFileConfigurationResult (*file_change_fixture)(const app::FileConfigurationChange&)=nullptr;
app::LocalSecuritySnapshot (*file_levels_fixture)()=nullptr;
#endif
app::LocalFileConfigurationSnapshot query_file_configuration(State& s,const app::LocalFileConfigurationQuery& q){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(file_query_fixture)return file_query_fixture(q);
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline&&!s.navigation_rendering)return win32::offline_file_configuration_snapshot(s.root,s.owner,q);
    if(s.mode!=State::ConnectionMode::online&&!s.navigation_rendering)return {};app::LocalControlFrame response;
    const auto payload=app::encode_file_configuration_query(q);if(payload.empty())return {app::LocalAccountResultCode::invalid_request};
    if(!online_exchange(s,request(s,app::LocalControlMessageType::file_configuration_query_request,payload),app::LocalControlMessageType::file_configuration_query_response,response))return {};
    return app::decode_file_configuration_snapshot(response.payload).value_or(app::LocalFileConfigurationSnapshot{});
}
app::LocalFileConfigurationResult change_file_configuration(State& s,const app::FileConfigurationChange& c){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(file_change_fixture)return file_change_fixture(c);
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_change_file_configuration(s.root,s.owner,c);
    if(s.mode!=State::ConnectionMode::online)return {};app::LocalControlFrame response;
    const auto payload=app::encode_file_configuration_change(c);if(payload.empty())return {app::LocalAccountResultCode::invalid_request};
    if(!online_exchange(s,request(s,app::LocalControlMessageType::file_configuration_change_request,payload),app::LocalControlMessageType::file_configuration_change_response,response))return {};
    return app::decode_file_configuration_result(response.payload).value_or(app::LocalFileConfigurationResult{});
}
std::optional<app::FileConfiguration> file_configuration_part(State& s,app::FileConfigurationPart part,app::FileGroupId group={}){
    // One bounded native offline snapshot, not up to84 separate ownership/DB
    // reopen cycles. Online requests retain revision-bound 64-KiB paging.
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(!file_query_fixture)
#endif
    if(s.mode==State::ConnectionMode::offline&&!s.system_recovery_only&&!s.navigation_rendering){const auto full=win32::offline_file_configuration_catalog(s.root,s.owner);if(!full||!full.value)return {};auto value=*full.value;if(group.value)value.areas.erase(std::remove_if(value.areas.begin(),value.areas.end(),[&](const auto& a){return !(a.area.group==group);}),value.areas.end());return value;}
    app::FileConfiguration result;app::LocalFileConfigurationQuery q;q.part=part;q.group=group;
    for(unsigned page_index=0;page_index<84;++page_index){
        const auto page=query_file_configuration(s,q);
        if(page.code!=app::LocalAccountResultCode::ok||page.part!=part||!app::valid_file_configuration_snapshot(page)||
            (q.catalog_revision&&page.policy.catalog_revision!=q.catalog_revision))return {};
        result.policy=page.policy;result.groups.insert(result.groups.end(),page.groups.begin(),page.groups.end());
        result.areas.insert(result.areas.end(),page.areas.begin(),page.areas.end());result.security_rules.insert(result.security_rules.end(),page.security_rules.begin(),page.security_rules.end());
        if(result.groups.size()>999||result.areas.size()>999||result.security_rules.size()>999)return {};
        if(!page.next_after)return result;
        if(page.next_after<=q.after_id)return {};
        q.catalog_revision=page.policy.catalog_revision;q.after_id=page.next_after;
    }
    return {};
}
void file_configuration_enabled(State& s,bool available){
    for(int page_index=21;page_index<=25;++page_index)for(auto child=GetWindow(s.pages[page_index],GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        const auto id=GetDlgCtrlID(child);
        if(page_index==25&&id!=IDC_FILE_SCAN&&id!=IDC_FILE_RECONCILE&&id!=IDC_CLAM_SAVE)continue;
        const bool reload=id==IDC_FILE_RELOAD||id==IDC_FILE_POLICY_RELOAD||id==IDC_FILE_RULE_RELOAD||id==IDC_FILE_EXEMPT_RELOAD;
        EnableWindow(child,id==IDC_FILE_RECONCILE?s.file_editor.pending.has_value():reload?!s.file_editor.pending:(available&&!s.file_editor.pending));
    }
}
bool file_configuration_apply(State& s,app::FileConfigurationChange c,bool reconcile=false){
    auto& f=s.file_editor;
    if(f.pending&&!reconcile){set_status(s,L"Outcome unresolved — reconcile the existing file operation first.");return false;}
    if(!reconcile){try{win32::WindowsSecureIdentifierGenerator generator;c.operation=generator.generate();}catch(...){set_status(s,L"Unavailable — secure operation identity failed. Draft retained.");return false;}}
    if(!app::valid_file_configuration_change(c)){set_status(s,L"Refused — invalid file settings. Check names, paths, requirements and explicit limits; draft retained.");return false;}
    const auto result=change_file_configuration(s,c);
    if(result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure){
        f.pending=c;file_configuration_enabled(s,false);set_status(s,L"Outcome unresolved — file edits are frozen. Reconcile checks this same operation; do not repeat it.");return false;
    }
    f.pending.reset();file_configuration_enabled(s,true);file_configuration_editability(s);
    if(result.code!=app::LocalAccountResultCode::ok){set_status(s,L"Refused — "+(result.detail.empty()?std::wstring(L"file settings changed or were rejected. Reload and review."):utf8_to_wide(result.detail))+L" Draft retained.");return false;}
    refresh_file_configuration_page(s);
    if(s.current_page==21&&f.ready){
        app::FileGroupId target=c.group.id;const bool area_change=c.action==app::FileConfigurationAction::area||c.action==app::FileConfigurationAction::insert_area_before||c.action==app::FileConfigurationAction::insert_area_after||c.action==app::FileConfigurationAction::move_area_up||c.action==app::FileConfigurationAction::move_area_down;
        if(area_change)target=c.area.area.group;else if(c.action==app::FileConfigurationAction::group)target={result.result.identity};
        const auto group=std::find_if(f.groups.begin(),f.groups.end(),[&](const auto& g){return g.id==target;});
        if(group!=f.groups.end()){f.group_index=static_cast<int>(group-f.groups.begin());SendMessageW(f.group_list,LB_SETCURSEL,f.group_index,0);if(file_load_areas(s,target)){f.group=*group;f.area.reset();if(area_change){const auto area=std::find_if(f.areas.begin(),f.areas.end(),[&](const auto& a){return a.area.id.value==result.result.identity;});if(area!=f.areas.end()){f.area_index=static_cast<int>(area-f.areas.begin());SendMessageW(f.area_list,LB_SETCURSEL,f.area_index,0);f.area=*area;f.group.reset();}}file_catalog_fields(s);}}
    }remember_page(s);
    if(!f.ready){set_status(s,L"Applied — save confirmed, but refreshed settings are unavailable. Reload before another save; do not repeat the committed operation.");return true;}
    if(c.action==app::FileConfigurationAction::begin_group_delete&&!result.detail.empty()){set_status(s,utf8_to_wide(result.detail));return true;}
    set_status(s,result.result.replayed?L"Applied — the matching committed file configuration was reconciled.":L"Applied — file configuration saved and audited. Current file operations recheck it; existing payloads were not moved.");return true;
}
void file_counts_show(const app::FileCountRule& rule,HWND daily_on,HWND daily,HWND ratio_on,HWND down,HWND up){
    Button_SetCheck(daily_on,rule.daily_downloads?BST_CHECKED:BST_UNCHECKED);SetWindowTextW(daily,rule.daily_downloads?std::to_wstring(*rule.daily_downloads).c_str():L"");
    Button_SetCheck(ratio_on,rule.ratio?BST_CHECKED:BST_UNCHECKED);SetWindowTextW(down,rule.ratio?std::to_wstring(rule.ratio->downloads).c_str():L"");SetWindowTextW(up,rule.ratio?std::to_wstring(rule.ratio->uploads).c_str():L"");
}
std::optional<app::FileCountRule> file_counts_read(HWND daily_on,HWND daily,HWND ratio_on,HWND down,HWND up){
    app::FileCountRule rule;
    if(Button_GetCheck(daily_on)==BST_CHECKED){const auto n=message_number(daily,1000000);if(!n||!*n)return {};rule.daily_downloads=static_cast<std::uint32_t>(*n);}
    if(Button_GetCheck(ratio_on)==BST_CHECKED){const auto d=message_number(down,1000000),u=message_number(up,1000000);if(!d||!*d||!u||!*u)return {};rule.ratio=app::FileRatio{static_cast<std::uint32_t>(*d),static_cast<std::uint32_t>(*u)};}
    return rule;
}
void file_count_controls(State& s){
    auto& f=s.file_editor;const bool available=!f.pending;
    EnableWindow(f.daily,available&&Button_GetCheck(f.daily_on)==BST_CHECKED);EnableWindow(f.ratio_down,available&&Button_GetCheck(f.ratio_on)==BST_CHECKED);EnableWindow(f.ratio_up,available&&Button_GetCheck(f.ratio_on)==BST_CHECKED);
    const bool explicit_rule=available&&f.rule.level&&Button_GetCheck(f.rule_inherit)!=BST_CHECKED;
    for(const auto h:{f.rule_daily_on,f.rule_ratio_on,f.rule_daily_exempt,f.rule_ratio_exempt})EnableWindow(h,explicit_rule);
    EnableWindow(f.rule_daily,explicit_rule&&Button_GetCheck(f.rule_daily_on)==BST_CHECKED);EnableWindow(f.rule_ratio_down,explicit_rule&&Button_GetCheck(f.rule_ratio_on)==BST_CHECKED);EnableWindow(f.rule_ratio_up,explicit_rule&&Button_GetCheck(f.rule_ratio_on)==BST_CHECKED);
}
void file_configuration_editability(State& s){
    auto& f=s.file_editor;const bool area=f.area.has_value(),selected=area||f.group.has_value();
    const bool deleting=f.group_index>=0&&static_cast<std::size_t>(f.group_index)<f.groups.size()&&f.groups[f.group_index].deletion_operation.valid();
    for(const auto h:{f.name,f.order,f.access,f.enabled})EnableWindow(h,selected&&!f.pending&&!deleting);
    for(const auto h:{f.upload,f.download,f.parent,f.path,f.browse,f.description,f.uploads,f.sysop_downloads,f.own_edit,f.backup_contents,f.ratio_uploads,f.ratio_downloads,f.area_cap,f.review})EnableWindow(h,area&&!f.pending&&!deleting);
    EnableWindow(f.sysop_downloads,area&&f.area->area.kind==app::FileAreaKind::ordinary&&!f.pending&&!deleting);EnableWindow(f.review,area&&f.area->area.kind==app::FileAreaKind::ordinary&&!f.pending&&!deleting);
    EnableWindow(f.order,selected&&!f.pending&&!deleting&&!f.insert_anchor.value);EnableWindow(f.parent,area&&!f.pending&&!deleting&&!f.insert_anchor.value);
    for(const auto id:{IDC_FILE_INSERT_BEFORE,IDC_FILE_INSERT_AFTER,IDC_FILE_MOVE_UP,IDC_FILE_MOVE_DOWN})EnableWindow(GetDlgItem(s.pages[21],id),area&&f.area->area.id.value&&!f.pending&&!deleting);
    const auto delete_group=GetDlgItem(s.pages[21],IDC_FILE_DELETE_GROUP);
    const bool offline=s.mode==State::ConnectionMode::offline;
    EnableWindow(delete_group,offline&&f.group_index>=0&&!f.pending);
    SetWindowTextW(delete_group,!offline?L"Stop BBS to Delete":deleting?L"Continue Deletion":L"Delete Group");
    EnableWindow(f.exempt_daily,f.exemption&&!f.pending);EnableWindow(f.exempt_ratio,f.exemption&&!f.pending);file_count_controls(s);
}
void file_catalog_fields(State& s){
    auto& f=s.file_editor;f.populating=true;const bool area=f.area.has_value();
    const auto set=[](HWND h,std::int64_t value){SetWindowTextW(h,std::to_wstring(value).c_str());};
    const auto& a=area?f.area->area:app::FileArea{};
    SetWindowTextW(f.name_label,area?L"Area Name":L"Group Name");
    SetWindowTextW(f.name,utf8_to_wide(area?a.name:f.group?f.group->name:std::string{}).c_str());set(f.order,area?a.order:f.group?f.group->order:0);
    set(f.access,area?a.permissions.access:f.group?f.group->access:1);set(f.upload,a.permissions.upload);set(f.download,a.permissions.download);
    SetWindowTextW(f.description,utf8_to_wide(a.description).c_str());SetWindowTextW(f.path,area?utf8_to_wide(f.area->storage_path).c_str():L"");
    SetWindowTextW(f.area_cap,a.maximum_upload_bytes?std::to_wstring(*a.maximum_upload_bytes).c_str():L"");
    Button_SetCheck(f.enabled,(area?a.enabled:f.group?f.group->enabled:false)?BST_CHECKED:BST_UNCHECKED);
    for(const auto& pair:std::vector<std::pair<HWND,bool>>{{f.uploads,a.permissions.uploads_enabled},{f.sysop_downloads,a.permissions.downloads_sysop_only},{f.own_edit,a.own_description_edit},{f.backup_contents,a.backup_contents},{f.ratio_uploads,a.ratio_uploads},{f.ratio_downloads,a.ratio_downloads},{f.review,a.sysop_review}})Button_SetCheck(pair.first,pair.second?BST_CHECKED:BST_UNCHECKED);
    SendMessageW(f.parent,CB_SETCURSEL,static_cast<WPARAM>(-1),0);
    for(std::size_t n=0;n<f.groups.size();++n)if(area&&f.groups[n].id==a.group)SendMessageW(f.parent,CB_SETCURSEL,n,0);
    file_configuration_editability(s);
    const auto detail=area?L"Area "+std::to_wstring(a.number)+L" (permanent ID "+std::to_wstring(a.id.value)+L"). Blank maximum inherits the global maximum. Folder changes never move files; nonempty/pending areas cannot be redirected.":
        std::wstring(L"Group editor. Select an area for its separate access, upload and download requirements. Payload maintenance is in File Manager.");
    auto field_detail=area&&!a.id.value?(f.insert_anchor.value?std::wstring(f.insert_after?L"New Area: insert after the selected Area. Other Areas move automatically.":L"New Area: insert before the selected Area. Other Areas move automatically."):std::wstring(L"New File Area. Enter its name and choose its existing storage folder.")):detail;
    if(s.mode!=State::ConnectionMode::offline)field_detail=L"Stop the BBS and close File Manager before deleting a File Group.\r\n"+field_detail;
    SetWindowTextW(f.detail,field_detail.c_str());f.populating=false;remember_page(s);
}
bool file_load_areas(State& s,app::FileGroupId group){
    const auto loaded=file_configuration_part(s,app::FileConfigurationPart::areas,group);auto& f=s.file_editor;
    if(!loaded||loaded->policy.catalog_revision!=f.policy.catalog_revision){set_status(s,L"Refused — file catalog changed during selection. Reload for a consistent view.");return false;}
    f.areas=loaded->areas;std::stable_sort(f.areas.begin(),f.areas.end(),[](const auto& a,const auto& b){return a.area.order==b.area.order?a.area.id.value<b.area.id.value:a.area.order<b.area.order;});SendMessageW(f.area_list,LB_RESETCONTENT,0,0);f.area_index=-1;
    for(const auto& a:f.areas){const auto label=std::to_wstring(a.area.number)+L"  "+utf8_to_wide(a.area.name)+(a.area.enabled?L"":L" (disabled)");SendMessageW(f.area_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
    return true;
}
void file_rule_fields(State& s){
    auto& f=s.file_editor;const auto index=SendMessageW(f.rule_level,CB_GETCURSEL,0,0);f.rule_index=static_cast<int>(index);f.rule={};
    if(index>=0){f.rule.level=static_cast<std::uint16_t>(SendMessageW(f.rule_level,CB_GETITEMDATA,index,0));const auto saved=std::find_if(f.rules.begin(),f.rules.end(),[&](const auto& r){return r.level==f.rule.level;});if(saved!=f.rules.end())f.rule=*saved;}
    Button_SetCheck(f.rule_inherit,f.rule.inherit?BST_CHECKED:BST_UNCHECKED);Button_SetCheck(f.rule_daily_exempt,f.rule.daily_exempt?BST_CHECKED:BST_UNCHECKED);Button_SetCheck(f.rule_ratio_exempt,f.rule.ratio_exempt?BST_CHECKED:BST_UNCHECKED);
    file_counts_show(f.rule.counts,f.rule_daily_on,f.rule_daily,f.rule_ratio_on,f.rule_ratio_down,f.rule_ratio_up);file_count_controls(s);remember_page(s);
}
void file_find_accounts(State& s,bool next=false){
    auto& f=s.file_editor;if(next)f.account_query.offset+=32;else f.account_query.offset=0;
    f.account_query.name_prefix=wide_to_utf8(text(f.exempt_filter)).value_or("");f.accounts=query_accounts(s,f.account_query);f.exemption.reset();f.account_index=-1;
    SendMessageW(f.exempt_select,CB_RESETCONTENT,0,0);for(const auto& a:f.accounts.accounts){const auto label=std::to_wstring(a.account_id)+L"  "+utf8_to_wide(a.display_name.value_or(""));SendMessageW(f.exempt_select,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
    for(const auto h:{f.exempt_daily,f.exempt_ratio}){Button_SetCheck(h,BST_UNCHECKED);EnableWindow(h,FALSE);}
    EnableWindow(GetDlgItem(s.pages[24],IDC_FILE_EXEMPT_NEXT),f.accounts.code==app::LocalAccountResultCode::ok&&f.accounts.has_more);
    SetWindowTextW(f.exempt_detail,f.accounts.code==app::LocalAccountResultCode::ok?L"Choose a caller. Exemptions affect file-count policies only; they do not fabricate upload/download statistics or bypass area permissions.":L"Caller lookup unavailable. No exemptions changed.");remember_page(s);
}
void file_choose_account(State& s,int index){
    auto& f=s.file_editor;if(index<0||static_cast<std::size_t>(index)>=f.accounts.accounts.size())return;
    app::LocalFileConfigurationQuery q;q.part=app::FileConfigurationPart::caller_exemption;q.account=f.accounts.accounts[static_cast<std::size_t>(index)].account_id;
    const auto result=query_file_configuration(s,q);f.account_index=index;f.exemption=result.code==app::LocalAccountResultCode::ok?result.exemption:std::nullopt;
    if(f.exemption)f.policy=result.policy;
    Button_SetCheck(f.exempt_daily,f.exemption&&f.exemption->daily_exempt?BST_CHECKED:BST_UNCHECKED);Button_SetCheck(f.exempt_ratio,f.exemption&&f.exemption->ratio_exempt?BST_CHECKED:BST_UNCHECKED);
    EnableWindow(f.exempt_daily,f.exemption&&!f.pending);EnableWindow(f.exempt_ratio,f.exemption&&!f.pending);remember_page(s);
    if(!f.exemption)set_status(s,L"Unavailable — exact caller exemption could not be read; nothing changed.");
}
void refresh_file_configuration_page(State& s){
    auto& f=s.file_editor;if(f.pending)return;f.ready=false;
    const auto policy=query_file_configuration(s,{});const bool available=policy.code==app::LocalAccountResultCode::ok;file_configuration_enabled(s,available);
    if(!available){set_status(s,L"Unavailable — the protected Runtime/offline file configuration could not be read. Reload to retry.");return;}f.policy=policy.policy;
    if(s.current_page==21){
        f.insert_anchor={};f.insert_revision=0;
        const auto groups=file_configuration_part(s,app::FileConfigurationPart::groups);if(!groups){file_configuration_enabled(s,false);set_status(s,L"Unavailable — complete group catalog could not be read consistently.");return;}
        f.policy=groups->policy;f.groups=groups->groups;std::stable_sort(f.groups.begin(),f.groups.end(),[](const auto& a,const auto& b){return a.order==b.order?a.id.value<b.id.value:a.order<b.order;});
        SendMessageW(f.group_list,LB_RESETCONTENT,0,0);SendMessageW(f.parent,CB_RESETCONTENT,0,0);f.group_index=-1;f.area_index=-1;f.group.reset();f.area.reset();
        for(const auto& g:f.groups){const auto caption=utf8_to_wide(g.name)+(g.enabled?L"":L" (disabled)");SendMessageW(f.group_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));SendMessageW(f.parent,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));}
        if(!f.groups.empty()){f.group_index=0;f.group=f.groups[0];SendMessageW(f.group_list,LB_SETCURSEL,0,0);if(!file_load_areas(s,f.group->id)){file_configuration_enabled(s,false);return;}}
        file_catalog_fields(s);
    }else if(s.current_page==22){
        SetWindowTextW(f.policy_cap,std::to_wstring(f.policy.maximum_upload_bytes).c_str());Button_SetCheck(f.lightbar,f.policy.lightbar_default?BST_CHECKED:BST_UNCHECKED);
        file_counts_show(f.policy.counts,f.daily_on,f.daily,f.ratio_on,f.ratio_down,f.ratio_up);file_count_controls(s);
        SetWindowTextW(f.policy_detail,win32::windows7_no_av()?L"Maximum applies to ordinary caller uploads, not Sysop imports. Limits count successful files, not bytes; the BBS-local day is used. Payload backups are configured per area and default Off.":L"Maximum applies to ordinary caller uploads, not Sysop imports. Limits count successful files, not bytes; the BBS-local day is used. ClamAV must be provisioned by Setup; unavailable/incomplete scans HOLD uploads privately. Payload backups are configured per area and default Off.");
    }else if(s.current_page==25){
        Button_SetCheck(f.scan,f.policy.scan_uploads?BST_CHECKED:BST_UNCHECKED);
    }else if(s.current_page==23){
        const auto rules=file_configuration_part(s,app::FileConfigurationPart::security_rules);if(!rules){file_configuration_enabled(s,false);set_status(s,L"Unavailable — explicit file rules could not be read consistently.");return;}
        f.policy=rules->policy;f.rules=rules->security_rules;
#ifdef SFX_ALPHA_TEST_AUTOMATION
        if(file_levels_fixture)s.security_catalog=file_levels_fixture();else
#endif
        refresh_levels(s);SendMessageW(f.rule_level,CB_RESETCONTENT,0,0);
        if(s.security_catalog.code!=app::LocalAccountResultCode::ok||s.security_catalog.has_more){file_configuration_enabled(s,false);set_status(s,L"Unavailable — complete current security levels could not be read.");return;}
        for(const auto& level:s.security_catalog.levels)if(level.state!=app::SecurityLevelState::retired){const auto caption=std::to_wstring(level.level)+L"  "+utf8_to_wide(level.label);const auto n=SendMessageW(f.rule_level,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));SendMessageW(f.rule_level,CB_SETITEMDATA,n,level.level);}
        SendMessageW(f.rule_level,CB_SETCURSEL,0,0);file_rule_fields(s);
        SetWindowTextW(f.rule_detail,L"Inherit uses the global count limits from Files > Transfer Policy. Explicit Off disables that limit for this level. Exemptions are separate. Area access/download requirements always apply. No default ratio is invented.");
    }else if(s.current_page==24)file_find_accounts(s);
    f.ready=true;remember_page(s);
}
std::wstring file_configuration_path_spelling(const std::filesystem::path& root,std::string_view saved,std::wstring_view candidate){
    const auto encoded=wide_to_utf8(candidate);
    if(saved.empty()||!encoded||!app::valid_file_configuration_path(saved)||!app::valid_file_configuration_path(*encoded))return std::wstring(candidate);
    auto before=std::filesystem::u8path(saved),after=std::filesystem::path(std::wstring(candidate));
    if(!before.is_absolute())before=root/before;if(!after.is_absolute())after=root/after;
    before.make_preferred();after.make_preferred();const auto& a=before.native();const auto& b=after.native();
    // Only equivalent already-valid spellings, not . or .. resolution, aliases,
    // link traversal or native identity assertions. The host still validates
    // every genuinely changed folder and retains its capability through CAS.
    return a.size()<=INT_MAX&&b.size()<=INT_MAX&&CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL?utf8_to_wide(saved):std::wstring(candidate);
}
bool save_file_configuration_page(State& s){
    auto& f=s.file_editor;if(!f.ready||f.pending){set_status(s,L"Unavailable — reload a complete current file configuration, or reconcile the pending operation first. Draft retained.");return false;}
    app::FileConfigurationChange c;c.expected_catalog_revision=f.policy.catalog_revision;c.policy=f.policy;
    if(s.current_page==21){
        const auto name=wide_to_utf8(text(f.name));const auto order=message_number(f.order,INT64_MAX-1),access=message_number(f.access,999);
        if(!name||!order||!access||!*access){set_status(s,L"Refused — enter a name, nonnegative display order and access level 1–999. Draft retained.");return false;}
        if(f.area){c.action=app::FileConfigurationAction::area;c.area=*f.area;auto& a=c.area.area;a.name=*name;a.order=*order;a.permissions.access=static_cast<std::uint16_t>(*access);
            const auto upload=message_number(f.upload,999),download=message_number(f.download,999);const auto parent=SendMessageW(f.parent,CB_GETCURSEL,0,0);const auto path=wide_to_utf8(file_configuration_path_spelling(s.root,f.area->storage_path,text(f.path)));
            if(!upload||!*upload||!download||!*download||parent<0||static_cast<std::size_t>(parent)>=f.groups.size()||!path){set_status(s,L"Refused — choose a parent, safe existing folder and upload/download requirements 1–999.");return false;}
            a.permissions.upload=static_cast<std::uint16_t>(*upload);a.permissions.download=static_cast<std::uint16_t>(*download);a.group=f.groups[static_cast<std::size_t>(parent)].id;c.area.storage_path=*path;
            const auto description=text(f.description);const auto encoded_description=wide_to_utf8(description);if(!description.empty()&&!encoded_description){set_status(s,L"Refused — the description contains invalid Unicode; draft retained.");return false;}
            a.description=encoded_description.value_or("");a.enabled=Button_GetCheck(f.enabled)==BST_CHECKED;a.permissions.uploads_enabled=Button_GetCheck(f.uploads)==BST_CHECKED;a.permissions.downloads_sysop_only=Button_GetCheck(f.sysop_downloads)==BST_CHECKED;
            a.own_description_edit=Button_GetCheck(f.own_edit)==BST_CHECKED;a.backup_contents=Button_GetCheck(f.backup_contents)==BST_CHECKED;a.ratio_uploads=Button_GetCheck(f.ratio_uploads)==BST_CHECKED;a.ratio_downloads=Button_GetCheck(f.ratio_downloads)==BST_CHECKED;a.sysop_review=Button_GetCheck(f.review)==BST_CHECKED;
            a.maximum_upload_bytes.reset();if(!text(f.area_cap).empty()){const auto maximum=message_number(f.area_cap,static_cast<std::int64_t>(app::kMaximumCallerFileBytes));if(!maximum||!*maximum){set_status(s,L"Refused — maximum is blank for inheritance, or 1–1,073,741,824 bytes.");return false;}a.maximum_upload_bytes=static_cast<std::uint64_t>(*maximum);}
            if(f.insert_anchor.value){c.action=f.insert_after?app::FileConfigurationAction::insert_area_after:app::FileConfigurationAction::insert_area_before;c.anchor=f.insert_anchor;c.anchor_revision=f.insert_revision;}
        }else if(f.group){c.action=app::FileConfigurationAction::group;c.group=*f.group;c.group.name=*name;c.group.order=*order;c.group.access=static_cast<std::uint16_t>(*access);c.group.enabled=Button_GetCheck(f.enabled)==BST_CHECKED;}
        else{set_status(s,L"Refused — select a group/area or choose New first.");return false;}
    }else if(s.current_page==22){
        const auto maximum=message_number(f.policy_cap,static_cast<std::int64_t>(app::kMaximumCallerFileBytes));const auto counts=file_counts_read(f.daily_on,f.daily,f.ratio_on,f.ratio_down,f.ratio_up);
        if(!maximum||!*maximum||!counts){set_status(s,L"Refused — enter the maximum bytes and every enabled count/ratio value explicitly; 1–1,000,000 per count.");return false;}
        c.policy.maximum_upload_bytes=static_cast<std::uint64_t>(*maximum);c.policy.counts=*counts;c.policy.lightbar_default=Button_GetCheck(f.lightbar)==BST_CHECKED;
    }else if(s.current_page==25){
        c.policy.scan_uploads=Button_GetCheck(f.scan)==BST_CHECKED;
    }else if(s.current_page==23){
        c.action=app::FileConfigurationAction::security_rule;c.security=f.rule;c.security.inherit=Button_GetCheck(f.rule_inherit)==BST_CHECKED;
        if(!c.security.inherit){const auto counts=file_counts_read(f.rule_daily_on,f.rule_daily,f.rule_ratio_on,f.rule_ratio_down,f.rule_ratio_up);if(!counts){set_status(s,L"Refused — enter valid explicit values for each enabled file-count rule.");return false;}c.security.counts=*counts;c.security.daily_exempt=Button_GetCheck(f.rule_daily_exempt)==BST_CHECKED;c.security.ratio_exempt=Button_GetCheck(f.rule_ratio_exempt)==BST_CHECKED;}
    }else if(s.current_page==24){if(!f.exemption){set_status(s,L"Refused — first choose a caller with a current exemption snapshot.");return false;}c.action=app::FileConfigurationAction::caller_exemption;c.exemption=*f.exemption;c.exemption.daily_exempt=Button_GetCheck(f.exempt_daily)==BST_CHECKED;c.exemption.ratio_exempt=Button_GetCheck(f.exempt_ratio)==BST_CHECKED;}
    else return false;
    return file_configuration_apply(s,c);
}
void file_browse_area(State& s){
    auto& f=s.file_editor;if(!f.area||f.pending||!IsWindowEnabled(f.path))return;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized)){set_status(s,L"Unavailable — Windows folder picker could not initialize.");return;}
    struct ReleaseCom{~ReleaseCom(){CoUninitialize();}} release_com;
    IFileDialog* raw{};if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)))){set_status(s,L"Unavailable — Windows folder picker could not open.");return;}
    const std::unique_ptr<IFileDialog,void(*)(IFileDialog*)> dialog(raw,[](IFileDialog* p){p->Release();});
    DWORD options{};if(FAILED(raw->GetOptions(&options))||FAILED(raw->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_DONTADDTORECENT|FOS_NOCHANGEDIR)))return;
    raw->SetTitle(L"Choose an existing file-area folder. SFX will not move or take ownership of its contents.");
    auto initial=std::filesystem::path(text(f.path));if(!initial.empty()){if(!initial.is_absolute())initial=s.root/initial;IShellItem* folder{};if(SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(),nullptr,IID_PPV_ARGS(&folder)))){raw->SetFolder(folder);folder->Release();}}
    if(FAILED(raw->Show(s.window)))return;IShellItem* item{};if(FAILED(raw->GetResult(&item)))return;PWSTR selected{};const auto status=item->GetDisplayName(SIGDN_FILESYSPATH,&selected);item->Release();
    if(SUCCEEDED(status)&&selected){SetWindowTextW(f.path,file_configuration_path_spelling(s.root,f.area->storage_path,selected).c_str());set_status(s,L"Selected — Save Changes validates the existing folder. No payloads have been moved or adopted.");}CoTaskMemFree(selected);
}
void open_file_manager(State& s){
    if(s.system_recovery_only||s.file_editor.pending||(s.clam_job&&!s.clam_job->done)||
       (s.clam_maintenance&&WaitForSingleObject(s.clam_maintenance.get(),0)==WAIT_TIMEOUT)){
        set_status(s,L"Finish or reconcile the current operation before opening File Manager.");return;
    }
    if(!resolve_dirty(s)||!load_editor_state(s))return;
    try{
        // Config owns no persistent offline storage lease. Its supported
        // read/save guards are released before this separate owner starts.
        const auto receipt=install::read_receipt(s.root,s.owner);install::verify_structure(receipt);
        install::require(receipt.program_count>install::file_manager_program_index,"file_manager_update_sfx_first");
        const auto existing=installed_window(receipt,L"SFXFileManagerWindow",install::file_manager_program_index);
        if(existing.result){ShowWindow(existing.result,SW_RESTORE);SetForegroundWindow(existing.result);set_status(s,L"File Manager is already open for this installation.");return;}
        const auto exe=s.root/L"Bin"/install::programs[install::file_manager_program_index];
        auto pin=install::open_object(exe,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        install::require(install::file_sha256(exe)==receipt.program_hashes[install::file_manager_program_index],"file_manager_image_changed");
        SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.hwnd=s.window;launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
        launch.lpFile=exe.c_str();launch.lpVerb=L"open";launch.nShow=SW_SHOWNORMAL;
        install::require(ShellExecuteExW(&launch)&&launch.hProcess,"file_manager_launch_failed");install::Handle process(launch.hProcess);
        set_status(s,L"File Manager opened for this installation. It verifies its own online/offline ownership.");
    }catch(const std::exception& e){set_status(s,L"File Manager could not open — "+utf8_to_wide(e.what())+L". Use matching Setup Repair; no alternate executable was searched for.");}
}
#include "file_group_order_actions.inc"
bool file_configuration_command(State& s,UINT id,UINT notification){
    if(id==IDC_FILE_MANAGER){if(notification==BN_CLICKED)open_file_manager(s);return true;}
    if(id<IDC_FILE_GROUPS||id>IDC_FILE_MOVE_DOWN)return false;auto& f=s.file_editor;if(f.populating)return true;
    if(id==IDC_FILE_RECONCILE){if(f.pending)file_configuration_apply(s,*f.pending,true);return true;}
    if(f.pending)return true;
    if(id==IDC_FILE_GROUPS&&notification==LBN_SELCHANGE){
        const auto index=static_cast<int>(SendMessageW(f.group_list,LB_GETCURSEL,0,0));if(index<0||static_cast<std::size_t>(index)>=f.groups.size())return true;const auto target=f.groups[static_cast<std::size_t>(index)].id;
        if(!resolve_dirty(s)){SendMessageW(f.group_list,LB_SETCURSEL,f.group_index,0);return true;}
        const auto group=std::find_if(f.groups.begin(),f.groups.end(),[&](const auto& g){return g.id==target;});if(group==f.groups.end()){set_status(s,L"Refused — selected group changed; reload.");return true;}
        if(!file_load_areas(s,target))return true;f.group_index=static_cast<int>(group-f.groups.begin());SendMessageW(f.group_list,LB_SETCURSEL,f.group_index,0);f.group=*group;f.area.reset();f.insert_anchor={};file_catalog_fields(s);return true;
    }
    if(id==IDC_FILE_AREAS&&notification==LBN_SELCHANGE){
        const auto index=static_cast<int>(SendMessageW(f.area_list,LB_GETCURSEL,0,0));if(index<0||static_cast<std::size_t>(index)>=f.areas.size())return true;const auto target=f.areas[static_cast<std::size_t>(index)].area;
        if(!resolve_dirty(s)){SendMessageW(f.area_list,LB_SETCURSEL,f.area_index,0);return true;}
        const auto group=std::find_if(f.groups.begin(),f.groups.end(),[&](const auto& g){return g.id==target.group;});if(group==f.groups.end()||!file_load_areas(s,target.group))return true;
        const auto area=std::find_if(f.areas.begin(),f.areas.end(),[&](const auto& a){return a.area.id==target.id;});if(area==f.areas.end()){set_status(s,L"Refused — selected area changed; reload.");return true;}
        f.group_index=static_cast<int>(group-f.groups.begin());f.area_index=static_cast<int>(area-f.areas.begin());SendMessageW(f.group_list,LB_SETCURSEL,f.group_index,0);SendMessageW(f.area_list,LB_SETCURSEL,f.area_index,0);f.area=*area;f.group.reset();f.insert_anchor={};file_catalog_fields(s);return true;
    }
    if(id==IDC_FILE_RULE_LEVEL&&notification==CBN_SELCHANGE){
        const auto index=SendMessageW(f.rule_level,CB_GETCURSEL,0,0);const auto target=SendMessageW(f.rule_level,CB_GETITEMDATA,index,0);
        if(!resolve_dirty(s)){SendMessageW(f.rule_level,CB_SETCURSEL,f.rule_index,0);return true;}
        int selected=-1;for(int n=0;n<SendMessageW(f.rule_level,CB_GETCOUNT,0,0);++n)if(SendMessageW(f.rule_level,CB_GETITEMDATA,n,0)==target){selected=n;break;}
        SendMessageW(f.rule_level,CB_SETCURSEL,selected,0);file_rule_fields(s);return true;
    }
    if(id==IDC_FILE_EXEMPT_SELECT&&notification==CBN_SELCHANGE){
        const auto index=static_cast<int>(SendMessageW(f.exempt_select,CB_GETCURSEL,0,0));if(index<0||static_cast<std::size_t>(index)>=f.accounts.accounts.size())return true;const auto target=f.accounts.accounts[static_cast<std::size_t>(index)].account_id;
        if(!resolve_dirty(s)){SendMessageW(f.exempt_select,CB_SETCURSEL,f.account_index,0);return true;}
        auto selected=std::find_if(f.accounts.accounts.begin(),f.accounts.accounts.end(),[&](const auto& a){return a.account_id==target;});
        if(selected==f.accounts.accounts.end()){
            app::LocalAccountQuery q;q.account_id=target;auto exact=query_accounts(s,q);if(exact.code!=app::LocalAccountResultCode::ok||exact.accounts.size()!=1||exact.accounts[0].account_id!=target){set_status(s,L"Refused — chosen caller is no longer available; search again.");return true;}
            f.accounts=std::move(exact);SendMessageW(f.exempt_select,CB_RESETCONTENT,0,0);const auto caption=std::to_wstring(target)+L"  "+utf8_to_wide(f.accounts.accounts[0].display_name.value_or(""));SendMessageW(f.exempt_select,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));selected=f.accounts.accounts.begin();
        }
        const auto chosen=static_cast<int>(selected-f.accounts.accounts.begin());SendMessageW(f.exempt_select,CB_SETCURSEL,chosen,0);file_choose_account(s,chosen);return true;
    }
    if(notification!=BN_CLICKED)return true;
    switch(id){
    case IDC_FILE_SAVE:case IDC_FILE_POLICY_SAVE:case IDC_FILE_RULE_SAVE:case IDC_FILE_EXEMPT_SAVE:save_file_configuration_page(s);break;
    case IDC_FILE_RELOAD:case IDC_FILE_POLICY_RELOAD:case IDC_FILE_RULE_RELOAD:case IDC_FILE_EXEMPT_RELOAD:if(resolve_dirty(s))refresh_file_configuration_page(s);break;
    case IDC_FILE_NEW_GROUP:if(resolve_dirty(s)){f.insert_anchor={};f.group_index=-1;f.area.reset();f.group=app::FileGroup{};f.group->order=static_cast<std::int64_t>(f.groups.size());file_catalog_fields(s);}break;
    case IDC_FILE_NEW_AREA:if(resolve_dirty(s)){if(f.group_index<0||static_cast<std::size_t>(f.group_index)>=f.groups.size()){set_status(s,L"Select a parent group first.");break;}if(f.groups[f.group_index].deletion_operation.valid()){set_status(s,L"Finish this Group deletion before adding Areas.");break;}f.insert_anchor={};f.group.reset();f.area=app::FileAreaConfiguration{};f.area->area.group=f.groups[static_cast<std::size_t>(f.group_index)].id;for(const auto& a:f.areas)f.area->area.order=std::max(f.area->area.order,(a.area.order<INT64_MAX?a.area.order+1:INT64_MAX-1));file_catalog_fields(s);}break;
    case IDC_FILE_INSERT_BEFORE:case IDC_FILE_INSERT_AFTER:if(resolve_dirty(s))file_prepare_insert(s,id==IDC_FILE_INSERT_AFTER);break;
    case IDC_FILE_MOVE_UP:case IDC_FILE_MOVE_DOWN:if(resolve_dirty(s))file_move_area(s,id==IDC_FILE_MOVE_DOWN);break;
    case IDC_FILE_DELETE_GROUP:if(resolve_dirty(s))file_delete_group(s);break;
    case IDC_FILE_BROWSE:file_browse_area(s);break;
    case IDC_FILE_DAILY_ON:case IDC_FILE_RATIO_ON:case IDC_FILE_RULE_INHERIT:case IDC_FILE_RULE_DAILY_ON:case IDC_FILE_RULE_RATIO_ON:file_count_controls(s);break;
    case IDC_FILE_EXEMPT_FIND:if(resolve_dirty(s))file_find_accounts(s);break;
    case IDC_FILE_EXEMPT_NEXT:if(resolve_dirty(s)&&f.accounts.has_more&&f.account_query.offset<999999)file_find_accounts(s,true);break;
    default:break;
    }
    return true;
}
void create_file_configuration_pages(State& s){
    auto& f=s.file_editor;
    const auto button=[&](HWND p,const wchar_t* caption,int x,int y,int width,int id){return control(s,p,L"BUTTON",caption,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,28,id);};
    const auto check=[&](HWND p,const wchar_t* caption,int x,int y,int width,int id){return control(s,p,L"BUTTON",caption,WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,24,id);};
    const auto edit=[&](HWND p,const wchar_t* caption,int x,int y,int width,int id,int maximum,bool numeric=false){label(s,p,caption,x,y,width,20);const auto h=control(s,p,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(numeric?ES_NUMBER:0),x,y+20,width,25,id,WS_EX_CLIENTEDGE);SendMessageW(h,EM_SETLIMITTEXT,maximum,0);return h;};
    const auto actions=[&](HWND p,int y,int save,int reload){button(p,L"Save Changes",12,y,150,save);button(p,L"Reload",174,y,150,reload);button(p,L"Reconcile",336,y,170,IDC_FILE_RECONCILE);};
    auto p=s.pages[21]=page(s);label(s,p,L"Groups",12,8,185,20);label(s,p,L"Areas (permanent numbers)",212,8,294,20);
    f.group_list=control(s,p,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL,12,32,185,130,IDC_FILE_GROUPS,WS_EX_CLIENTEDGE);f.area_list=control(s,p,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL,212,32,294,130,IDC_FILE_AREAS,WS_EX_CLIENTEDGE);
    button(p,L"New Group",12,172,150,IDC_FILE_NEW_GROUP);button(p,L"New Area",174,172,150,IDC_FILE_NEW_AREA);
    button(p,L"Open File Manager",336,172,170,IDC_FILE_MANAGER);
    button(p,L"Delete Group",12,208,150,IDC_FILE_DELETE_GROUP);button(p,L"Insert Before",174,208,80,IDC_FILE_INSERT_BEFORE);button(p,L"Insert After",262,208,80,IDC_FILE_INSERT_AFTER);button(p,L"Move Up",350,208,72,IDC_FILE_MOVE_UP);button(p,L"Move Down",430,208,76,IDC_FILE_MOVE_DOWN);
    f.name_label=control(s,p,L"STATIC",L"Group Name",0,12,248,320,20,IDC_FILE_NAME_LABEL);f.name=control(s,p,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,12,268,320,25,IDC_FILE_NAME,WS_EX_CLIENTEDGE);SendMessageW(f.name,EM_SETLIMITTEXT,64,0);f.order=edit(p,L"Display order",350,248,156,IDC_FILE_ORDER,18,true);
    f.access=edit(p,L"Access minimum",12,304,150,IDC_FILE_ACCESS,3,true);f.upload=edit(p,L"Upload minimum",184,304,150,IDC_FILE_UPLOAD,3,true);f.download=edit(p,L"Download minimum",356,304,150,IDC_FILE_DOWNLOAD,3,true);
    label(s,p,L"Parent group",12,360,310,20);f.parent=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,382,320,240,IDC_FILE_PARENT);f.enabled=check(p,L"Enabled",350,382,156,IDC_FILE_ENABLED);
    f.path=edit(p,L"Existing storage folder (relative to SFX or absolute)",12,418,380,IDC_FILE_PATH,2048);f.browse=button(p,L"Browse...",404,438,102,IDC_FILE_BROWSE);
    f.description=edit(p,L"Area description",12,474,530,IDC_FILE_DESCRIPTION,256);
    f.uploads=check(p,L"Allow caller uploads",12,530,230,IDC_FILE_UPLOADS);f.sysop_downloads=check(p,L"Sysop-only downloads",266,530,240,IDC_FILE_SYSOP_DOWNLOADS);
    f.own_edit=check(p,L"Caller may edit own descriptions",12,562,310,IDC_FILE_OWN_EDIT);f.backup_contents=check(p,L"Back up contents",336,562,170,IDC_FILE_BACKUP_CONTENTS);
    f.ratio_uploads=check(p,L"Uploads participate in ratio",12,594,240,IDC_FILE_RATIO_UPLOADS);f.ratio_downloads=check(p,L"Downloads participate in ratio",266,594,240,IDC_FILE_RATIO_DOWNLOADS);
    f.review=check(p,L"Route accepted caller uploads to Uploads for Sysop review",12,626,530,IDC_FILE_REVIEW);f.area_cap=edit(p,L"Caller upload maximum bytes (blank = inherit)",12,662,530,IDC_FILE_AREA_CAP,10,true);
    actions(p,722,IDC_FILE_SAVE,IDC_FILE_RELOAD);f.detail=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,764,530,80,0,WS_EX_CLIENTEDGE);
    p=s.pages[22]=page(s);label(s,p,L"Global caller file-transfer defaults. Existing saved caller choices remain intact.",12,8,494,44);
    f.policy_cap=edit(p,L"Maximum caller upload bytes (1–1,073,741,824)",12,64,494,IDC_FILE_POLICY_CAP,10,true);f.lightbar=check(p,L"Light-bar selection default for new callers",12,124,494,IDC_FILE_LIGHTBAR);
    if(win32::clamav_available())label(s,p,L"Caller-upload scanning and Portable ClamAV maintenance: Security > BBS Security > ClamAV.",12,156,494,42);
    f.daily_on=check(p,L"Limit successful downloads per BBS-local day",12,208,494,IDC_FILE_DAILY_ON);f.daily=edit(p,L"Daily files (1–1,000,000)",12,240,220,IDC_FILE_DAILY,7,true);
    f.ratio_on=check(p,L"Enable file-count ratio (explicit values required)",12,312,494,IDC_FILE_RATIO_ON);f.ratio_down=edit(p,L"Downloads",12,344,230,IDC_FILE_RATIO_DOWN,7,true);f.ratio_up=edit(p,L"Per uploads",266,344,240,IDC_FILE_RATIO_UP,7,true);
    actions(p,412,IDC_FILE_POLICY_SAVE,IDC_FILE_POLICY_RELOAD);f.policy_detail=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,460,494,132,0,WS_EX_CLIENTEDGE);
    p=s.pages[23]=page(s);label(s,p,L"File-count overrides for existing security levels. Level definitions remain in Users & Access > Security Levels.",12,8,494,48);
    label(s,p,L"Security level",12,68,494,20);f.rule_level=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,92,494,240,IDC_FILE_RULE_LEVEL);f.rule_inherit=check(p,L"Inherit global file-count policy",12,132,494,IDC_FILE_RULE_INHERIT);
    f.rule_daily_on=check(p,L"Limit successful daily downloads",12,180,494,IDC_FILE_RULE_DAILY_ON);f.rule_daily=edit(p,L"Daily files (1–1,000,000)",12,212,230,IDC_FILE_RULE_DAILY,7,true);
    f.rule_ratio_on=check(p,L"Enable file-count ratio",12,276,494,IDC_FILE_RULE_RATIO_ON);f.rule_ratio_down=edit(p,L"Downloads",12,308,230,IDC_FILE_RULE_RATIO_DOWN,7,true);f.rule_ratio_up=edit(p,L"Per uploads",266,308,240,IDC_FILE_RULE_RATIO_UP,7,true);
    f.rule_daily_exempt=check(p,L"Exempt this level from daily download counts",12,372,494,IDC_FILE_RULE_DAILY_EXEMPT);f.rule_ratio_exempt=check(p,L"Exempt this level from file-count ratios",12,404,494,IDC_FILE_RULE_RATIO_EXEMPT);
    actions(p,452,IDC_FILE_RULE_SAVE,IDC_FILE_RULE_RELOAD);f.rule_detail=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,500,494,100,0,WS_EX_CLIENTEDGE);
    p=s.pages[24]=page(s);label(s,p,L"Caller-specific count exemptions. These do not grant access to private areas or waive file permissions.",12,8,494,48);
    f.exempt_filter=edit(p,L"Find caller (current-name prefix)",12,68,320,IDC_FILE_EXEMPT_FILTER,30);button(p,L"Find",350,88,72,IDC_FILE_EXEMPT_FIND);button(p,L"Next",434,88,72,IDC_FILE_EXEMPT_NEXT);
    label(s,p,L"Caller",12,132,494,20);f.exempt_select=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,156,494,240,IDC_FILE_EXEMPT_SELECT);
    f.exempt_daily=check(p,L"Exempt caller from daily download file counts",12,212,494,IDC_FILE_EXEMPT_DAILY);f.exempt_ratio=check(p,L"Exempt caller from file-count ratios",12,252,494,IDC_FILE_EXEMPT_RATIO);
    actions(p,308,IDC_FILE_EXEMPT_SAVE,IDC_FILE_EXEMPT_RELOAD);f.exempt_detail=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,356,494,100,0,WS_EX_CLIENTEDGE);
}
