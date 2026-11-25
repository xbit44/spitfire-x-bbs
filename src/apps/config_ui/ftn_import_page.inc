bool ftn_import_busy(const State& s){return s.ftn.import_job&&!s.ftn.import_job->done.load();}
void ftn_import_counts(State& s){
    auto& f=s.ftn;const auto selected=std::count_if(f.import_list.entries.begin(),f.import_list.entries.end(),[](const auto& e){return e.selected;});
    const auto count=L"Selected: "+std::to_wstring(selected)+L" of "+std::to_wstring(f.import_list.entries.size())+L" usable. Shown: "+std::to_wstring(f.import_visible.size())+L". Local conference slots free: "+std::to_wstring(999-f.local_echoes.size())+L". Rejected/duplicate lines: "+std::to_wstring(f.import_list.issues.size());SetWindowTextW(f.import_count,count.c_str());
}
void ftn_import_rows(State& s){
    auto& f=s.ftn;f.populating=true;SendMessageW(f.import_rows,WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(f.import_rows);f.import_visible.clear();const auto find=ftn_fold(door_text(f.import_find));std::vector<std::size_t> matches;
    for(std::size_t i=0;i<f.import_list.entries.size();++i)if(find.empty()||ftn_fold(f.import_list.entries[i].tag+" "+f.import_list.entries[i].description).find(find)!=std::string::npos)matches.push_back(i);
    if(f.import_offset>=matches.size())f.import_offset=0;const auto end=std::min(matches.size(),f.import_offset+500);
    for(auto i=f.import_offset;i<end;++i){const auto index=matches[i];const auto& e=f.import_list.entries[index];const auto row=static_cast<int>(f.import_visible.size());f.import_visible.push_back(index);auto tag=utf8_to_wide(e.tag);LVITEMW item{};item.mask=LVIF_TEXT|LVIF_PARAM;item.iItem=row;item.pszText=tag.data();item.lParam=index;ListView_InsertItem(f.import_rows,&item);auto description=utf8_to_wide(e.description);ListView_SetItemText(f.import_rows,row,1,description.data());ListView_SetCheckState(f.import_rows,row,e.selected);}
    EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_PREVIOUS),f.import_offset!=0);EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_NEXT),end<matches.size());SendMessageW(f.import_rows,WM_SETREDRAW,TRUE,0);InvalidateRect(f.import_rows,nullptr,TRUE);f.populating=false;ftn_import_counts(s);
}
std::optional<std::string> ftn_list_utf8(std::string bytes,bool cp437){
    if(bytes.size()>=3&&static_cast<unsigned char>(bytes[0])==0xef&&static_cast<unsigned char>(bytes[1])==0xbb&&static_cast<unsigned char>(bytes[2])==0xbf)bytes.erase(0,3);
    if(!cp437)return bytes;if(bytes.empty())return bytes;const auto count=MultiByteToWideChar(437,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);if(count<=0)return {};std::wstring wide(static_cast<std::size_t>(count),L'\0');if(MultiByteToWideChar(437,0,bytes.data(),static_cast<int>(bytes.size()),wide.data(),count)!=count)return {};return wide_to_utf8(wide);
}
void ftn_import_loaded(State& s,std::string bytes){
    auto& f=s.ftn;const auto converted=ftn_list_utf8(std::move(bytes),SendMessageW(f.import_encoding,CB_GETCURSEL,0,0)==1);if(!converted){set_status(s,L"The list encoding could not be read. Previous list and draft remain.");return;}
    auto list=app::parse_ftn_echo_list(*converted);if(!list.complete){set_status(s,L"The list exceeds the supported size or line limits. No entries were imported.");return;}f.import_list=std::move(list);f.import_requirements.assign(f.import_list.entries.size(),{});f.import_offset=0;f.import_changed=true;ftn_import_rows(s);remember_page(s);std::wstring message=L"List loaded for review. All usable entries start checked. Nothing has been saved or subscribed.";
    if(!f.import_list.issues.empty()){message+=L" Rejected lines:";for(std::size_t i=0;i<std::min<std::size_t>(f.import_list.issues.size(),5);++i)message+=L" "+std::to_wstring(f.import_list.issues[i].line)+L" ("+utf8_to_wide(f.import_list.issues[i].reason)+L")";}set_status(s,message);
}
void ftn_import_poll(State& s){
    auto& f=s.ftn;if(!f.import_job||!f.import_job->done.load())return;const auto job=std::move(f.import_job);if(job->thread.joinable())job->thread.join();KillTimer(s.window,config_timer::ftn_import);win32::FtnEchoListDownload result;{std::lock_guard<std::mutex> lock(job->mutex);result=std::move(job->result);}EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_LOAD),TRUE);EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_BROWSE),TRUE);
    if(result.code==win32::FtnEchoListDownloadCode::ok)ftn_import_loaded(s,std::move(result.bytes));else set_status(s,utf8_to_wide(std::string(win32::ftn_echo_list_download_message(result.code))));
}
void ftn_import_load(State& s){
    auto& f=s.ftn;if(ftn_import_busy(s))return;const auto source=SendMessageW(f.import_source,CB_GETCURSEL,0,0);if(source==2){set_status(s,L"EchoList skipped. You can finish a valid network without importing echoes.");return;}
    const auto path=door_text(f.import_path);
    if(source==0){if(!app::valid_ftn_echo_list_url(path)){set_status(s,L"Enter an explicit HTTP or HTTPS list URL without credentials.");return;}
        auto job=std::make_shared<FtnEditorState::ImportJob>();f.import_job=job;EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_LOAD),FALSE);EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_BROWSE),FALSE);SetTimer(s.window,config_timer::ftn_import,100,nullptr);set_status(s,path.compare(0,7,"http://")==0?L"Downloading the explicitly selected plain HTTP list. Cancel stops the download.":L"Downloading the list over verified HTTPS. Cancel stops the download.");
        job->thread=std::thread([job,path]{win32::FtnEchoListDownload result;try{result=win32::download_ftn_echo_list(path,job->cancel);}catch(...){result.code=win32::FtnEchoListDownloadCode::transport;}{std::lock_guard<std::mutex> lock(job->mutex);job->result=std::move(result);}job->done=true;});return;
    }
    const auto wide=utf8_to_wide(path);install::Handle input(CreateFileW(wide.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!input||!GetFileInformationByHandle(input.get(),&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(input.get(),&size)||size.QuadPart<0||size.QuadPart>4*1024*1024){set_status(s,L"Choose an existing ordinary .NA text file no larger than 4 MiB. Previous list and draft remain.");return;}
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');DWORD read{};if(!ReadFile(input.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size()){set_status(s,L"The complete list could not be read. Previous list and draft remain.");return;}ftn_import_loaded(s,std::move(bytes));
}
void ftn_import_browse(State& s){
    if(ftn_import_busy(s))return;std::array<wchar_t,32768> buffer{};OPENFILENAMEW picker{};picker.lStructSize=sizeof(picker);picker.hwndOwner=s.window;picker.lpstrFile=buffer.data();picker.nMaxFile=static_cast<DWORD>(buffer.size());picker.lpstrFilter=L"Echo lists (*.na;*.txt)\0*.na;*.txt\0All files\0*.*\0\0";picker.lpstrTitle=L"Choose Message Echo List";picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(GetOpenFileNameW(&picker)){SendMessageW(s.ftn.import_source,CB_SETCURSEL,1,0);SetWindowTextW(s.ftn.import_path,buffer.data());set_status(s,L"List selected. Choose Load List to review its entries.");}else if(CommDlgExtendedError())set_status(s,L"The file picker failed. Previous source and selections remain.");SetFocus(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_BROWSE));
}
void ftn_import_requirements(State& s){
    unsigned access{},read{},post{};auto& f=s.ftn;if(!door_number(f.import_access,access,999)||!access||!door_number(f.import_read,read,999)||!read||!door_number(f.import_post,post,999)||!post){set_status(s,L"Enter Access, Read and Post levels from 1 to 999.");return;}std::size_t count{};for(std::size_t i=0;i<f.import_list.entries.size();++i)if(f.import_list.entries[i].selected){f.import_requirements[i]={static_cast<std::uint16_t>(access),static_cast<std::uint16_t>(read),static_cast<std::uint16_t>(post)};++count;}if(count)f.import_changed=true;set_status(s,L"Requirements set for "+std::to_wstring(count)+L" checked entries across all pages. Existing conferences keep their current requirements.");
}
app::FtnEchoImportDestination ftn_import_destination(State& s){
    const auto& f=s.ftn;const auto* c=f.import_base?&*f.import_base:ftn_current(s);
    if(!c||f.network<0||static_cast<std::size_t>(f.network)>=c->networks.size())return {0,{},{{"import.group","unknown_network"}}};
    std::vector<std::pair<std::int64_t,std::string>> groups;for(const auto& g:f.message_groups)groups.emplace_back(g.id.value,g.name);
    std::vector<std::pair<std::int64_t,std::int64_t>> echoes;for(const auto& e:f.local_echoes)echoes.emplace_back(e.id.value,e.group.value);
    return app::resolve_ftn_echo_import_destination(*c,c->networks[f.network].code,groups,echoes);
}
bool ftn_import_destination_label(State& s){
    const auto destination=ftn_import_destination(s);const auto* c=s.ftn.import_base?&*s.ftn.import_base:ftn_current(s);
    const auto name=c&&s.ftn.network>=0&&static_cast<std::size_t>(s.ftn.network)<c->networks.size()?utf8_to_wide(c->networks[s.ftn.network].name):L"Unavailable";
    const auto label=L"Network: "+name+L"\r\nMessage group: "+(destination.issues.empty()?utf8_to_wide(destination.name)+(destination.group?L"":L" (created when you apply)"):L"Review existing message mappings");
    SetWindowTextW(s.ftn.import_group_label,label.c_str());return destination.issues.empty();
}
bool ftn_import_reload(State& s){
    auto& f=s.ftn;if(f.pending||ftn_import_busy(s))return false;
    if(f.wizard||!f.import_base||!f.config||app::encode_ftn_configuration(*f.import_base)!=app::encode_ftn_configuration(*f.config)){
        set_status(s,L"Finish or cancel your other FTN draft before reloading saved settings. Your selections remain.");return false;
    }
    const auto network=f.import_base->networks[f.network].code;const auto saved=query_ftn(s);
    if(!saved){set_status(s,L"Saved FTN settings could not be loaded. Your selections and requirements remain.");return false;}
    const auto found=std::find_if(saved->networks.begin(),saved->networks.end(),[&](const auto& n){return n.code==network;});
    if(found==saved->networks.end()){set_status(s,L"This network was removed from saved settings. Cancel this import and choose a saved network. Your selections remain.");return false;}
    f.config=*saved;f.draft=*saved;f.import_base=*saved;f.draft_changed=false;f.network=static_cast<int>(found-saved->networks.begin());
    if(!ftn_catalogs(s)){set_status(s,L"Saved message or file areas changed again. Reload Saved Settings and review. Your selections remain.");return false;}
    ftn_import_destination_label(s);set_status(s,L"Saved settings reloaded. Echo selections and entered requirements kept. Review / Apply Selected to save.");return true;
}
bool ftn_import_apply(State& s){
    auto& f=s.ftn;if(ftn_import_busy(s)||f.pending||f.network<0)return false;
    if(!f.catalog_ready||!f.import_base){set_status(s,L"Not imported. Reload Saved Settings and review before applying. Your selections remain.");return false;}
    const auto* c=&*f.import_base;const auto destination=ftn_import_destination(s);
    if(!destination.issues.empty()){set_status(s,L"Not imported. This network's existing message group or echo mappings need review. Reload Saved Settings; no groups or echoes were moved.");return false;}
    if(std::none_of(f.import_list.entries.begin(),f.import_list.entries.end(),[](const auto& e){return e.selected;})){set_status(s,L"Choose at least one echo to import, or Discard Changes to leave without importing.");return false;}
    const auto plan=app::plan_ftn_echo_import(*c,c->networks[f.network].code,f.import_list,f.import_requirements,f.local_echoes.size(),destination.group,destination.group?std::string{}:destination.name,door_checked(f.import_rename));
    if(!plan.issues.empty()){set_status(s,plan.issues.front().field=="import.capacity"?L"Not imported. Checked entries exceed the remaining local conference slots. Clear All and select fewer entries; no partial import was saved.":L"Not imported. Review selected names, group and security requirements. Long echo tags need an explicit shorter local name in the normal editor.");return false;}
    for(const auto& echo:plan.configuration.message_echoes)if(echo.network==c->networks[f.network].code&&echo.store_locally&&!echo.local_echo&&destination.group){
        const auto name=echo.local_name.empty()?echo.tag:echo.local_name;
        if(std::any_of(f.local_echoes.begin(),f.local_echoes.end(),[&](const auto& existing){return existing.group.value==destination.group&&ftn_fold(existing.name)==ftn_fold(name);})){set_status(s,L"Not imported. Message echo '"+utf8_to_wide(name)+L"' already exists in this group. Use Message Echoes to review its mapping before importing this tag. Your selections remain.");return false;}
    }
    const auto review=L"Checked entries: "+std::to_wstring(plan.selected)+L"\nNew local conferences: "+std::to_wstring(plan.created)+L"\nExisting echoes unchanged: "+std::to_wstring(plan.unchanged)+L"\nReviewed names/descriptions to update: "+std::to_wstring(plan.text_updates)+L"\n\nNo missing-list entries will be deleted. No AreaFix request will be generated.";
    if(!ftn_confirm(s,L"Review Selected Echo Import",review))return false;f.import_changed=true;f.echo=-1;
    if(f.wizard){f.draft=plan.configuration;f.draft_changed=true;f.import_changed=false;f.import_base.reset();activate_page(s,30);ftn_network_fill(s);remember_page(s);set_status(s,L"Selected echoes are retained in the setup draft. Review and Finish to save them atomically.");return true;}
    // The import editor remains the draft after a refusal. Never feed a
    // failed plan back in as though its unallocated echoes were saved ones.
    if(!ftn_commit(s,plan.configuration)){
        if(f.last_save_conflict&&!f.pending)set_status(s,L"Not imported. Saved settings, names or mappings conflict. Choose Reload Saved Settings, then review and apply. Your selections and requirements remain.");
        return false;
    }
    f.import_changed=false;f.import_base.reset();f.draft=*f.config;ftn_catalogs(s);activate_page(s,32);ftn_echo_fill(s);remember_page(s);set_status(s,L"Selected echoes imported and saved. No AreaFix request was sent.");return true;
}
void ftn_open_import(State& s){
    auto& f=s.ftn;if(!ftn_catalogs(s))return;const auto* c=ftn_current(s);if(!c||f.network<0)return;f.import_base=*c;const auto& n=f.import_base->networks[f.network];f.import_list={};f.import_requirements.clear();f.import_offset=0;f.import_changed=false;SendMessageW(f.import_source,CB_SETCURSEL,n.echo_list_source==app::FtnEchoListSource::local_file?1:0,0);door_set(f.import_path,n.echo_list_source==app::FtnEchoListSource::local_file?n.echo_list_file:n.echo_list_url);SendMessageW(f.import_encoding,CB_SETCURSEL,0,0);ftn_import_destination_label(s);EnableWindow(GetDlgItem(s.pages[35],IDC_FTN_IMPORT_RELOAD),!f.wizard);door_check(f.import_rename,false);for(auto h:{f.import_access,f.import_read,f.import_post})door_set_number(h,10);activate_page(s,35);ftn_import_rows(s);remember_page(s);
}
void create_ftn_import(State& s){
    auto& f=s.ftn;FtnControls ui{s,s.pages[35]=page(s)};ui.label(L"Import Message Echo List",12,8);f.import_source=ui.combo(L"List source",12,42,240,IDC_FTN_IMPORT_SOURCE);for(auto title:{L"Download (HTTP or HTTPS)",L"Browse a local .NA file",L"Skip for now"})SendMessageW(f.import_source,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));f.import_encoding=ui.combo(L"Text encoding",270,42,252,IDC_FTN_IMPORT_ENCODING);for(auto title:{L"UTF-8 / ASCII",L"DOS CP437"})SendMessageW(f.import_encoding,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    f.import_path=ui.edit(L"EchoList URL or local file",12,106,390,IDC_FTN_IMPORT_PATH,2048);ui.button(L"Browse...",414,128,108,IDC_FTN_IMPORT_BROWSE);ui.button(L"Load List",12,170,156,IDC_FTN_IMPORT_LOAD);ui.button(L"Cancel Download",180,170,174,IDC_FTN_IMPORT_CANCEL);ui.button(L"Back",366,170,156,IDC_FTN_IMPORT_BACK);
    f.import_find=ui.edit(L"Find echo tag or description",12,213,510,IDC_FTN_IMPORT_SEARCH,128);ui.button(L"Select All Usable",12,273,240,IDC_FTN_IMPORT_SELECT);ui.button(L"Clear All Usable",270,273,252,IDC_FTN_IMPORT_CLEAR);
    f.import_rows=control(s,ui.parent,WC_LISTVIEWW,L"",WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SHOWSELALWAYS,12,315,510,178,IDC_FTN_IMPORT_ROWS);ListView_SetExtendedListViewStyle(f.import_rows,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);int column{};for(auto pair:std::array<std::pair<const wchar_t*,int>,2>{{{L"Echo tag",185},{L"Description",310}}}){LVCOLUMNW c{};c.mask=LVCF_WIDTH|LVCF_TEXT;c.cx=MulDiv(pair.second,s.dpi,96);c.pszText=const_cast<wchar_t*>(pair.first);ListView_InsertColumn(f.import_rows,column++,&c);}
    ui.button(L"Previous 500",12,505,156,IDC_FTN_IMPORT_PREVIOUS);ui.button(L"Next 500",180,505,156,IDC_FTN_IMPORT_NEXT);f.import_count=ui.label(L"No list loaded",12,547,510,42);
    f.import_group_label=control(s,ui.parent,L"STATIC",L"",SS_LEFT,12,602,320,52,IDC_FTN_IMPORT_GROUP);ui.button(L"Reload Saved Settings",342,613,180,IDC_FTN_IMPORT_RELOAD);
    f.import_access=ui.edit(L"Access",12,664,150,IDC_FTN_IMPORT_ACCESS,3);f.import_read=ui.edit(L"Read",180,664,150,IDC_FTN_IMPORT_READ,3);f.import_post=ui.edit(L"Post",348,664,174,IDC_FTN_IMPORT_POST,3);ui.button(L"Set Requirements for Checked Entries",12,726,510,IDC_FTN_IMPORT_REQUIREMENTS);
    f.import_rename=ui.check(L"Update checked existing names / descriptions (default: add new only)",12,769,510,IDC_FTN_IMPORT_RENAME);ui.button(L"Review / Apply Selected",12,817,510,IDC_FTN_IMPORT_APPLY);
}
