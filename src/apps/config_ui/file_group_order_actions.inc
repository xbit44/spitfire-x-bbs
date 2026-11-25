void file_prepare_insert(State& s,bool after){
    auto& f=s.file_editor;if(!f.area||!f.area->area.id.value)return;
    const auto selected=*f.area;f.insert_anchor=selected.area.id;f.insert_revision=selected.area.revision;f.insert_after=after;
    f.group.reset();f.area=app::FileAreaConfiguration{};f.area->area.group=selected.area.group;
    file_catalog_fields(s);SetFocus(f.name);
}
void file_move_area(State& s,bool down){
    auto& f=s.file_editor;if(!f.area||!f.area->area.id.value)return;
    app::FileConfigurationChange c;c.action=down?app::FileConfigurationAction::move_area_down:app::FileConfigurationAction::move_area_up;c.area=*f.area;c.expected_catalog_revision=f.policy.catalog_revision;
    file_configuration_apply(s,c);
}
void file_delete_group(State& s){
    auto& f=s.file_editor;if(f.group_index<0||static_cast<std::size_t>(f.group_index)>=f.groups.size())return;
    if(s.mode!=State::ConnectionMode::offline){set_status(s,L"Stop the BBS and close File Manager before deleting a File Group.");return;}
    app::FileConfigurationChange c;c.action=app::FileConfigurationAction::preview_group_delete;c.group=f.groups[f.group_index];c.expected_catalog_revision=f.policy.catalog_revision;
    try{win32::WindowsSecureIdentifierGenerator ids;c.operation=ids.generate();}catch(...){return;}
    set_status(s,L"Inspecting the selected Group and its Areas before deletion...");UpdateWindow(s.window);
    const auto preview=change_file_configuration(s,c);
    if(preview.code!=app::LocalAccountResultCode::ok){set_status(s,utf8_to_wide(preview.detail.empty()?"Group deletion could not be reviewed. Reload and review its current settings.":preview.detail));return;}
    bool disk=c.group.delete_disk_files;
    if(!c.group.deletion_operation.valid()){
        const auto heading=L"Delete File Group: "+utf8_to_wide(c.group.name);
        const auto content=L"This Group contains "+std::to_wstring(preview.area_count)+L" Area(s), "+std::to_wstring(preview.catalog_files)+L" file listing(s), and "+(preview.detail.empty()?std::to_wstring(preview.disk_files)+L" file(s) in its storage folders":std::wstring(L"disk contents not fully counted"))+L".\r\n\r\n"+utf8_to_wide(preview.detail)+L"\r\nChoose whether to keep or permanently delete files on disk. SFX retains internal history and permanent numbers. Empty storage folders remain.";
        TASKDIALOG_BUTTON choices[]={{101,L"Remove from SFX only - keep files on disk"},{102,L"Delete from SFX and delete files on disk"}};
        TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.hwndParent=s.window;dialog.dwFlags=TDF_SIZE_TO_CONTENT|TDF_ALLOW_DIALOG_CANCELLATION;dialog.dwCommonButtons=TDCBF_CANCEL_BUTTON;dialog.pszWindowTitle=L"Delete File Group";dialog.pszMainInstruction=heading.c_str();dialog.pszContent=content.c_str();dialog.pButtons=choices;dialog.cButtons=2;dialog.nDefaultButton=IDCANCEL;
        int choice{};if(FAILED(TaskDialogIndirect(&dialog,&choice,nullptr,nullptr))||(choice!=101&&choice!=102)){set_status(s,L"Cancelled - Group, Areas and files kept.");return;}disk=choice==102;
    }
    const auto confirm=(c.group.deletion_operation.valid()?L"Continue the saved deletion of ":L"Delete ")+utf8_to_wide(c.group.name)+L" and its Areas?\r\n\r\n"+(disk?L"Reviewed files will be permanently deleted from disk. Already completed deletions cannot be undone.":L"Remove the Group, Areas and file listings from SFX. Keep files on disk.");
    if(MessageBoxW(s.window,confirm.c_str(),L"Confirm File Group deletion",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES){set_status(s,L"Cancelled - no new deletion work started.");return;}
    c.action=app::FileConfigurationAction::begin_group_delete;c.delete_disk_files=disk;
    set_status(s,L"Removing the selected Group. Please wait for the result...");UpdateWindow(s.window);
    const auto done=change_file_configuration(s,c);
    refresh_file_configuration_page(s);
    if(done.code==app::LocalAccountResultCode::unavailable||done.code==app::LocalAccountResultCode::storage_failure){f.pending=c;file_configuration_enabled(s,false);set_status(s,L"Deletion outcome unresolved. Use Reconcile for this same request.");return;}
    set_status(s,utf8_to_wide(done.detail.empty()?(done.code==app::LocalAccountResultCode::ok?"Group removed.":"Group deletion was refused; reload and review."):done.detail));
}
