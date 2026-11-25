void State::enable(){
    const bool idle=available(),catalog=mode==Mode::catalog,record=idle&&catalog&&selected&&!selected->operation_pending&&!selected->purged;
    for(const auto& item:controls)EnableWindow(item.second,idle);
    for(const UINT id:{save,short_description,long_description,uploader,sysop_only})EnableWindow(control(id),record&&(id!=save||dirty));
    for(const UINT id:{view_text,view_zip,rename_file,copy_file,move_file,remove_file,restore_file,purge_file,mark_missing,reupload_block})EnableWindow(control(id),record);
    if(record){const auto state=selected->file.state;EnableWindow(control(restore_file),state!=app::FileCatalogState::available);EnableWindow(control(purge_file),state==app::FileCatalogState::removed);for(const UINT id:{view_text,view_zip,rename_file,copy_file,move_file,remove_file,mark_missing})EnableWindow(control(id),state==app::FileCatalogState::available);}
    EnableWindow(control(previous),idle&&catalog&&!previous_pages.empty());EnableWindow(control(next),idle&&((catalog||mode==Mode::inspection)?next_after.value!=0:(mode==Mode::operations||mode==Mode::imports||mode==Mode::intake)?pending_next.valid():mode==Mode::unregistered&&!discovery.finished));
    for(const UINT id:{import_files,import_folder,unregistered})EnableWindow(control(id),idle&&area.value>0&&catalog);
    const bool candidates=mode==Mode::unregistered||mode==Mode::import_preview;
    for(const UINT id:{select_all,preview_import,batch_options})EnableWindow(control(id),idle&&candidates&&!batch.empty());
    EnableWindow(control(confirm_import),idle&&mode==Mode::import_preview&&std::any_of(batch.begin(),batch.end(),[](const auto& row){return row.selected&&row.preview&&!row.added&&!row.held;}));
    const bool journal=mode==Mode::operations||mode==Mode::imports||mode==Mode::intake;
    const auto pending_index=ListView_GetNextItem(control(records),-1,LVNI_SELECTED);
    const auto* pending_row=journal&&pending_index>=0&&static_cast<std::size_t>(pending_index)<pending.size()?&pending[static_cast<std::size_t>(pending_index)]:nullptr;
    EnableWindow(control(reconcile),idle&&pending_row&&(mode!=Mode::intake||pending_row->state==static_cast<unsigned>(app::FileIntakeState::accepted)));
    EnableWindow(control(cancel_intent),idle&&pending_row&&mode!=Mode::intake);
    EnableWindow(control(purge_intake),idle&&pending_row&&mode==Mode::intake&&pending_row->state==static_cast<unsigned>(app::FileIntakeState::held));
    const bool tracked=worker&&!worker->busy()&&tracking.blocks_edits()&&!closing;
    EnableWindow(control(status_check),tracked);EnableWindow(control(retry_same),tracked&&tracking.state()==JobTracking::unknown);EnableWindow(control(cancel_job),tracked&&!cancel_sent);
    if(preview)preview->busy(!idle);if(closing)message(L"Closing owner connection normally; waiting for bounded work to finish...");
}
bool State::send(ClientCommand command){if(!worker)return false;command.serial=++serial;if(!worker->submit(command))return false;last_command=std::move(command);enable();return true;}
bool State::start(app::FileManagementTask task,Purpose why){if(!available()||!tracking.begin(task))return false;purpose=why;cancel_sent=false;ClientCommand request;request.kind=ClientCommandKind::start;request.task=std::move(task);if(!send(request)){tracking.link_failed();message(L"Start status is unknown. Keep this exact request; check status or retry SAME request.");enable();return false;}return true;}
void State::tick(){
    if(!worker)return;if(auto reply=worker->poll())receive(std::move(*reply));
    if(!window)return;
    if(closing&&!worker->busy()&&!disconnecting){disconnecting=true;ClientCommand request;request.kind=ClientCommandKind::disconnect;send(request);return;}
    if(!closing&&close_preview_wanted&&viewport&&available()){app::FileManagementTask task;task.job=fresh_id();task.kind=app::FileManagementTaskKind::close_view;task.target=viewport->cursor;close_preview_wanted=false;start(task,Purpose::view_close);return;}
    if(!closing&&tracking.state()==JobTracking::running&&!worker->busy()&&GetTickCount64()-last_poll>=500){last_poll=GetTickCount64();ClientCommand request;request.kind=ClientCommandKind::status;request.job=tracking.exact_task()->job;send(request);}
}
void State::receive(ClientReply reply){
    if(reply.serial!=last_command.serial)return;
    if(reply.kind==ClientCommandKind::disconnect){ready=false;DestroyWindow(window);return;}
    if(reply.kind==ClientCommandKind::connect||reply.kind==ClientCommandKind::configuration){ready=reply.connected&&reply.configuration.has_value();online=reply.online;if(ready){configuration=std::move(reply.configuration);populate_areas();read_catalog(true);}else message(wide(reply.diagnostic.empty()?"Owner connection unavailable. Close and reopen explicitly; no offline fallback was attempted.":reply.diagnostic));enable();return;}
    if(reply.kind==ClientCommandKind::read){
        if(reply.read_code!=app::LocalAccountResultCode::ok){message(wide(reply.diagnostic.empty()?"Read unavailable. Current edits remain unchanged; reload before acting on another record.":reply.diagnostic));after_save={};enable();return;}
        if(mode==Mode::catalog||mode==Mode::inspection){const auto keep=selected?selected->file.id:app::FileId{};rows=std::move(reply.records);next_after=reply.next;selected.reset();for(const auto& row:rows)if(row.file.id==keep)selected=row;if(keep.value&&!selected)after_save={};list();fields();}
        else{pending=std::move(reply.pending);pending_next=reply.pending_next;selected.reset();list();fields();}
        message(online?L"Connected to running BBS. Changes use the authenticated owner job service.":L"BBS offline. File Manager holds exclusive supported storage ownership.");
        if(after_save){auto action=std::move(after_save);after_save={};action();}enable();return;
    }
    const bool observed=tracking.observe(reply.status,reply.kind==ClientCommandKind::start);
    if(!observed){message(L"Job status UNKNOWN. New edits are frozen. Check status or retry the SAME request; do not assume it failed.");enable();return;}
    if(reply.status.code==app::LocalAccountResultCode::ok&&reply.status.state==app::FileManagementJobState::running){auto progress=wide(reply.status.detail);if(reply.status.bytes_total)progress+=L" Pass: "+std::to_wstring(reply.status.bytes_done)+L" / "+std::to_wstring(reply.status.bytes_total)+L" bytes.";message(progress);enable();return;}
    completed(reply.status);enable();
}
void State::populate_areas(){
    if(!configuration)return;refreshing=true;TreeView_DeleteAllItems(control(areas));HTREEITEM selected_item{};
    const auto add=[&](HTREEITEM parent,const std::wstring& name,LPARAM key){TVINSERTSTRUCTW item{};item.hParent=parent;item.hInsertAfter=TVI_LAST;item.item.mask=TVIF_TEXT|TVIF_PARAM;item.item.pszText=const_cast<wchar_t*>(name.c_str());item.item.lParam=key;return TreeView_InsertItem(control(areas),&item);};
    const auto all=add(TVI_ROOT,L"All file areas",-1);if(!area.value)selected_item=all;
    for(const auto& group:configuration->groups){const auto parent=add(TVI_ROOT,wide(group.name),0);for(std::size_t n=0;n<configuration->areas.size();++n){const auto& row=configuration->areas[n];if(!(row.area.group==group.id))continue;const auto item=add(parent,wide(row.area.name)+(row.area.enabled?L"":L" (disabled)"),static_cast<LPARAM>(n+1));if(row.area.id==area)selected_item=item;}TreeView_Expand(control(areas),parent,TVE_EXPAND);}
    if(selected_item)TreeView_SelectItem(control(areas),selected_item);else{area={};TreeView_SelectItem(control(areas),all);}refreshing=false;
}
void State::list(){
    refreshing=true;const auto list_control=control(records);ListView_DeleteAllItems(list_control);const bool candidates=mode==Mode::unregistered||mode==Mode::import_preview;ListView_SetExtendedListViewStyle(list_control,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|(candidates?LVS_EX_CHECKBOXES:0));
    const auto add=[&](int index,const std::array<std::wstring,4>& values,bool checked,bool picked){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=index;item.pszText=const_cast<wchar_t*>(values[0].c_str());ListView_InsertItem(list_control,&item);for(int n=1;n<4;++n)ListView_SetItemText(list_control,index,n,const_cast<wchar_t*>(values[static_cast<std::size_t>(n)].c_str()));if(candidates)ListView_SetCheckState(list_control,index,checked);if(picked)ListView_SetItemState(list_control,index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);};
    if(mode==Mode::catalog||mode==Mode::inspection){for(std::size_t n=0;n<rows.size();++n){const auto& row=rows[n];auto status=row.purged?L"Purged":row.operation_pending?L"Pending operation":row.file.state==app::FileCatalogState::removed?L"Recoverably removed":row.file.state==app::FileCatalogState::missing?L"Marked missing":L"Available";std::wstring outcome=status;if(mode==Mode::inspection&&n<presence.size()){static constexpr const wchar_t* labels[]={L"present and exact",L"missing",L"changed",L"unavailable",L"removed",L"pending"};outcome+=L" / ";outcome+=labels[static_cast<unsigned>(presence[n])];}add(static_cast<int>(n),{wide(row.file.name),std::to_wstring(row.file.bytes),wide(row.file.public_uploader),outcome},false,selected&&selected->file.id==row.file.id);}}
    else if(candidates){for(std::size_t n=0;n<batch.size();++n){const auto& row=batch[n];add(static_cast<int>(n),{wide(app::sanitize_file_text(row.source.filename().u8string(),255,false)),row.preview?std::to_wstring(row.preview->registration.payload.bytes):L"",row.preview?wide(row.preview->registration.public_uploader):L"",row.outcome},row.selected,false);}}
    else{for(std::size_t n=0;n<pending.size();++n){const auto& row=pending[n];add(static_cast<int>(n),{wide(row.filename),L"File "+std::to_wstring(row.file.value),L"Area "+std::to_wstring(row.area.value),wide(row.detail)},false,n==0);}}
    SetWindowTextW(control(information),candidates?(std::to_wstring(batch.size())+L" displayed candidates; no recursive import.").c_str():L"Stable catalog IDs; no automatic deletion.");refreshing=false;
}
void State::fields(){
    refreshing=true;SetWindowTextW(control(filename),selected?wide(selected->file.name).c_str():L"");
    SetWindowTextW(control(short_description),selected?wide(selected->file.descriptions.short_text).c_str():L"");
    SetWindowTextW(control(long_description),selected?native_form_lines(wide(selected->file.descriptions.long_text)).c_str():L"");
    SetWindowTextW(control(uploader),selected?wide(selected->file.public_uploader).c_str():L"");
    Button_SetCheck(control(sysop_only),selected&&selected->file.sysop_only?BST_CHECKED:BST_UNCHECKED);dirty=false;refreshing=false;enable();
}
void State::read_catalog(bool reset){if(!ready||!worker||worker->busy())return;if(reset){after={};previous_pages.clear();}mode=Mode::catalog;ClientCommand request;request.kind=ClientCommandKind::read;request.query.filter.area=area;request.query.filter.search=utf8(text(control(search)));request.query.filter.field=static_cast<app::FileManagementSearch>(std::max<LRESULT>(0,SendMessageW(control(search_field),CB_GETCURSEL,0,0)));const auto filter=SendMessageW(control(state_filter),CB_GETCURSEL,0,0);if(filter!=1)request.query.filter.state=filter==2?app::FileCatalogState::removed:filter==3?app::FileCatalogState::missing:app::FileCatalogState::available;request.query.after=after;send(request);}
void State::select_row(){
    if(refreshing||!available())return;if(mode==Mode::unregistered||mode==Mode::import_preview){for(std::size_t n=0;n<batch.size();++n)batch[n].selected=ListView_GetCheckState(control(records),static_cast<int>(n))!=0;enable();return;}
    if(mode!=Mode::catalog&&mode!=Mode::inspection){enable();return;}const auto index=ListView_GetNextItem(control(records),-1,LVNI_SELECTED);if(index<0||static_cast<std::size_t>(index)>=rows.size())return;const auto chosen=rows[static_cast<std::size_t>(index)];if(selected&&selected->file.id==chosen.file.id)return;dirty_gate([this,chosen]{selected=chosen;fields();});
}
bool State::dirty_gate(std::function<void()> action){if(!available())return false;if(!dirty){action();return true;}const auto answer=MessageBoxW(window,L"Save the current metadata edits before navigating?\nYes = Save, No = Discard, Cancel = Stay here.",L"Unsaved File Manager edits",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON3);if(answer==IDYES){after_save=std::move(action);save_metadata();return false;}if(answer==IDNO){dirty=false;action();return true;}list();populate_areas();return false;}
void State::save_metadata(){if(!available()||!selected||!dirty)return;app::FileManagementTask task;task.job=fresh_id();task.operation.operation=fresh_id();task.operation.file=selected->file.id;task.operation.expected_revision=selected->file.revision;task.operation.action=app::FileOperationAction::metadata;task.operation.descriptions={utf8(text(control(short_description))),multiline(control(long_description))};task.operation.public_uploader=utf8(text(control(uploader)));task.operation.sysop_only=Button_GetCheck(control(sysop_only))==BST_CHECKED;if(!app::valid_file_management_task(task)||(task.operation.public_uploader.empty()&&selected->file.origin!=app::FileOrigin::ftn)){message(L"Metadata is invalid or too long. Edits are retained; blank public attribution is allowed only for an FTN file.");after_save={};return;}start(task,Purpose::metadata);}
void State::completed(const app::FileManagementJobStatus& result){
    const bool success=result.code==app::LocalAccountResultCode::ok&&result.state==app::FileManagementJobState::completed;message(wide(result.detail.empty()?"Request refused; refresh current state before another action.":result.detail));
    if(purpose==Purpose::metadata){if(success){dirty=false;read_catalog(false);}else after_save={};return;}
    if(purpose==Purpose::view){if(success&&result.view){viewport=result.view;if(!close_preview_wanted&&!preview->show(*result.view)){close_preview_wanted=true;message(L"Preview window unavailable; closing its native source lease.");}}else{preview->close();viewport.reset();}return;}
    if(purpose==Purpose::view_close){preview->close();if(success)viewport.reset();return;}
    if(purpose==Purpose::discovery||purpose==Purpose::discovery_next){if(success&&result.discovery){discovery=*result.discovery;batch.clear();batch_area=discovery.area;std::filesystem::path folder=std::filesystem::u8path(discovery.source_folder);if(folder.empty()&&configuration){const auto found=std::find_if(configuration->areas.begin(),configuration->areas.end(),[&](const auto& a){return a.area.id==batch_area;});if(found!=configuration->areas.end()){folder=std::filesystem::u8path(found->storage_path);if(folder.is_relative())folder=root/folder;}}for(const auto& name:discovery.names){ImportRow row;row.source=folder/std::filesystem::u8path(name);row.operation=fresh_id();row.outcome=L"Candidate only; native preview required";batch.push_back(std::move(row));}mode=Mode::unregistered;selected.reset();list();fields();}return;}
    if(purpose==Purpose::inspection){if(success&&result.inspection){rows=result.inspection->catalog.records;presence=result.inspection->presence;next_after=result.inspection->catalog.next;selected.reset();mode=Mode::inspection;list();fields();}return;}
    if(purpose==Purpose::preview_import||purpose==Purpose::accept_import){if(batch_index>=batch.size()){batch_phase=BatchPhase::none;return;}auto& row=batch[batch_index];row.outcome=wide(result.detail);if(purpose==Purpose::preview_import){row.preview=success?result.preview:std::nullopt;row.duplicates=result.duplicate_contents;if(!row.preview)row.selected=false;++batch_index;list();next_import();return;}row.added=success;row.held=result.state==app::FileManagementJobState::held||result.state==app::FileManagementJobState::failed;if(!success){batch_phase=BatchPhase::none;list();message(L"Batch stopped at an unaccepted item. Added items stay added; pending/held items need explicit review. Nothing else was attempted.");return;}++batch_index;list();next_import();return;}
    if(purpose==Purpose::purge_preview){if(success&&result.purge_preview){const auto question=L"Permanently purge ONLY the verified held intake objects?\nExisting library files are not selected. This cannot be undone.";if(MessageBoxW(window,question,L"Confirm held-upload purge",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES){app::FileManagementTask task;task.job=fresh_id();task.kind=app::FileManagementTaskKind::purge_intake;task.purge=*result.purge_preview;start(task,Purpose::purge_accept);}}return;}
    if(mode==Mode::operations||mode==Mode::imports||mode==Mode::intake)pending_page();else if(success)read_catalog(false);
}
