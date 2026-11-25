// Compact native controls shared by the normal FTN editors and setup helper.
struct FtnControls {
    State& s;HWND parent;
    HWND label(const wchar_t* title,int x,int y,int width=510,int height=20){return ::label(s,parent,title,x,y,width,height);}
    HWND edit(const wchar_t* title,int x,int y,int width,int id,unsigned maximum=512){label(title,x,y,width);const auto h=control(s,parent,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,x,y+22,width,25,id);SendMessageW(h,EM_SETLIMITTEXT,maximum,0);return h;}
    HWND button(const wchar_t* title,int x,int y,int width,int id){return control(s,parent,L"BUTTON",title,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,28,id);}
    HWND check(const wchar_t* title,int x,int y,int width,int id){return control(s,parent,L"BUTTON",title,WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,25,id);}
    HWND combo(const wchar_t* title,int x,int y,int width,int id){label(title,x,y,width);return control(s,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,x,y+22,width,220,id);}
    HWND list(int x,int y,int width,int height,int id){return control(s,parent,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,x,y,width,height,id);}
};
constexpr wchar_t ftn_combo_ids_property[]=L"SFX.FTN.ComboIds.v1";
LRESULT CALLBACK ftn_combo_cleanup(HWND h,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR){if(message==WM_NCDESTROY){delete static_cast<std::vector<std::int64_t>*>(RemovePropW(h,ftn_combo_ids_property));RemoveWindowSubclass(h,ftn_combo_cleanup,id);}return DefSubclassProc(h,message,wparam,lparam);}
std::vector<std::int64_t>& ftn_combo_ids(HWND h){auto* ids=static_cast<std::vector<std::int64_t>*>(GetPropW(h,ftn_combo_ids_property));if(!ids){auto owned=std::make_unique<std::vector<std::int64_t>>();if(!SetPropW(h,ftn_combo_ids_property,owned.get()))throw std::runtime_error("FTN combo identity storage unavailable");if(!SetWindowSubclass(h,ftn_combo_cleanup,11000,0)){RemovePropW(h,ftn_combo_ids_property);throw std::runtime_error("FTN combo lifetime unavailable");}ids=owned.release();}return *ids;}
void ftn_combo_item(HWND h,const std::wstring& title,std::int64_t data){const auto row=SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));if(row!=CB_ERR&&row!=CB_ERRSPACE){auto& ids=ftn_combo_ids(h);if(row==0)ids.clear();if(static_cast<std::size_t>(row)!=ids.size())throw std::runtime_error("FTN combo order invalid");ids.push_back(data);}}
void ftn_combo_select(HWND h,std::int64_t data){const auto& ids=ftn_combo_ids(h);for(std::size_t row=0;row<ids.size();++row)if(ids[row]==data){SendMessageW(h,CB_SETCURSEL,row,0);return;}SendMessageW(h,CB_SETCURSEL,static_cast<WPARAM>(-1),0);}
std::int64_t ftn_combo_value(HWND h){const auto row=SendMessageW(h,CB_GETCURSEL,0,0);const auto& ids=ftn_combo_ids(h);return row<0||static_cast<std::size_t>(row)>=ids.size()?-1:ids[static_cast<std::size_t>(row)];}
std::string ftn_fold(std::string value){for(auto& ch:value)if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');return value;}
bool ftn_catalogs(State& s){
    auto& f=s.ftn;f.catalog_ready=false;f.message_groups.clear();f.local_echoes.clear();f.file_areas.clear();
    app::LocalMessageConfigurationQuery q;std::int64_t revision{};
    for(unsigned page=0;page<100;++page){const auto part=query_message_configuration(s,q);if(part.code!=app::LocalAccountResultCode::ok||(revision&&part.catalog.revision!=revision))return false;revision=part.catalog.revision;f.message_groups.insert(f.message_groups.end(),part.catalog.groups.begin(),part.catalog.groups.end());if(f.message_groups.size()>999)return false;if(!part.catalog.next)break;q.cursor=*part.catalog.next;if(page==99)return false;}
    for(const auto& group:f.message_groups){q={};q.group=group.id;q.cursor.configuration_revision=revision;for(unsigned page=0;page<100;++page){const auto part=query_message_configuration(s,q);if(part.code!=app::LocalAccountResultCode::ok||part.catalog.revision!=revision)return false;f.local_echoes.insert(f.local_echoes.end(),part.catalog.echoes.begin(),part.catalog.echoes.end());if(f.local_echoes.size()>999)return false;if(!part.catalog.next)break;q.cursor=*part.catalog.next;if(page==99)return false;}}
    const auto files=file_configuration_part(s,app::FileConfigurationPart::areas);if(!files)return false;f.file_areas=files->areas;
    const auto* c=f.draft?&*f.draft:f.config?&*f.config:nullptr;
    f.catalog_ready=c&&c->message_catalog_revision==revision&&c->file_catalog_revision==files->policy.catalog_revision;
    if(!f.catalog_ready)set_status(s,L"Message or file areas changed. Reload FTN settings before editing their mappings. Your draft remains.");return f.catalog_ready;
}
void ftn_group_choices(State& s,HWND h,std::int64_t selected){
    SendMessageW(h,CB_RESETCONTENT,0,0);ftn_combo_item(h,L"Create a new group",0);
    for(const auto& g:s.ftn.message_groups)ftn_combo_item(h,utf8_to_wide(g.name)+(g.enabled?L"":L" (disabled)"),g.id.value);ftn_combo_select(h,selected);
}
bool ftn_confirm(State& s,const std::wstring& title,const std::wstring& body){return MessageBoxW(s.window,body.c_str(),title.c_str(),MB_OKCANCEL|MB_ICONWARNING)==IDOK;}
void ftn_list_select(HWND h,int index){for(LRESULT row=0;row<SendMessageW(h,LB_GETCOUNT,0,0);++row)if(SendMessageW(h,LB_GETITEMDATA,row,0)==index){SendMessageW(h,LB_SETCURSEL,row,0);return;}SendMessageW(h,LB_SETCURSEL,static_cast<WPARAM>(-1),0);}
