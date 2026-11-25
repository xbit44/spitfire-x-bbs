// Shared owner administration, with Runtime's occupied-room recheck at commit.
#ifdef SFX_ALPHA_TEST_AUTOMATION
app::ChatConfigurationResult (*chat_query_fixture)()=nullptr;
app::ChatConfigurationResult (*chat_change_fixture)(const app::ChatConfiguration&)=nullptr;
#endif
app::ChatConfigurationResult query_chat(State& s){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(chat_query_fixture)return chat_query_fixture();
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline&&!s.navigation_rendering)return win32::offline_chat_configuration(s.root,s.owner);
    if(s.mode!=State::ConnectionMode::online&&!s.navigation_rendering)return {};
    app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::chat_configuration_query_request,{}),app::LocalControlMessageType::chat_configuration_query_response,response))return {};
    return app::decode_chat_result(response.payload).value_or(app::ChatConfigurationResult{});
}
app::ChatConfigurationResult apply_chat(State& s,const app::ChatConfiguration& value){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(chat_change_fixture)return chat_change_fixture(value);
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_change_chat_configuration(s.root,s.owner,value);
    if(s.mode!=State::ConnectionMode::online)return {};
    app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::chat_configuration_change_request,app::encode_chat_configuration(value)),app::LocalControlMessageType::chat_configuration_change_response,response))return {};
    return app::decode_chat_result(response.payload).value_or(app::ChatConfigurationResult{});
}
void chat_editor_enable(State& s,bool enabled){for(const auto h:{s.chat.name,s.chat.enabled,s.chat.mode,s.chat.level,s.chat.sysop})EnableWindow(h,enabled);}
void chat_command_enable(State& s,bool enabled){for(const auto h:{s.chat.command_menu,s.chat.command_door,s.chat.command_key,s.chat.command_name,s.chat.command_enabled,s.chat.command_mode,s.chat.command_level,s.chat.command_sysop})EnableWindow(h,enabled);}
void chat_fill(State& s){
    auto& c=s.chat;SendMessageW(c.list,LB_RESETCONTENT,0,0);SendMessageW(c.commands,LB_RESETCONTENT,0,0);
    if(!c.config)return;
    for(const auto& room:c.config->rooms){auto name=utf8_to_wide(room.name);if(room.id==c.config->entry)name+=L" (entry room)";if(!room.enabled)name+=L" - disabled";
        const auto row=SendMessageW(c.list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(c.list,LB_SETITEMDATA,row,room.id);}
    for(std::size_t n=0;n<c.config->commands.size();++n){const auto& command=c.config->commands[n];auto name=utf8_to_wide(std::string(app::menu_definition(command.menu).title)+" - "+command.key+"  "+command.name+(command.enabled?"":" (disabled)"));
        const auto row=SendMessageW(c.commands,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(c.commands,LB_SETITEMDATA,row,n);}
    Button_SetCheck(c.stats,c.config->exclude_sysop_statistics?BST_CHECKED:BST_UNCHECKED);
}
void refresh_chat(State& s){
    const auto result=query_chat(s);s.chat.config=result.configuration;s.chat.editing=false;s.chat.command_editing=false;chat_editor_enable(s,false);chat_command_enable(s,false);chat_fill(s);
    set_status(s,result.code==app::ChatResult::ok?L"Saved Chat settings loaded.":L"Chat settings are unavailable. No change was made.");
}
enum class ChatSaveTask { rooms, statistics, commands };
bool chat_commit(State& s,const app::ChatConfiguration& next,ChatSaveTask task=ChatSaveTask::rooms){
    if(!app::valid_chat_configuration(next)){set_status(s,L"Not saved. Check the name, security level, Door and unused command key. Your edits remain.");return false;}
    const auto result=apply_chat(s,next);
    if(result.code!=app::ChatResult::ok||!result.configuration){
        set_status(s,result.code==app::ChatResult::busy?L"This room is in use. Retry when it is empty. Your edits remain.":result.code==app::ChatResult::conflict?L"Settings changed elsewhere. Reload and review before saving. Your edits remain.":L"Save was not confirmed. Reload to check the saved settings before retrying. Your edits remain.");return false;
    }
    s.chat.config=*result.configuration;chat_fill(s);remember_page(s);
    if(task==ChatSaveTask::commands)set_status(s,L"Door commands saved. Available commands appear when callers next refresh that menu.");
    else if(task==ChatSaveTask::statistics)set_status(s,L"Statistics setting saved. It applies to future logons.");
    else set_status(s,s.mode==State::ConnectionMode::offline?L"Chat Rooms saved. They will be used when the BBS starts.":L"Chat Rooms saved and active.");
    return true;
}
app::ChatRoomId chat_selected_room(State& s){const auto row=SendMessageW(s.chat.list,LB_GETCURSEL,0,0);return row==LB_ERR?0:static_cast<app::ChatRoomId>(SendMessageW(s.chat.list,LB_GETITEMDATA,row,0));}
void chat_edit(State& s,bool add){
    if(!s.chat.config||!resolve_dirty(s))return;
    app::ChatRoom room;s.chat.room=0;
    if(!add){const auto id=chat_selected_room(s);const auto it=std::find_if(s.chat.config->rooms.begin(),s.chat.config->rooms.end(),[&](const auto& r){return r.id==id;});if(it==s.chat.config->rooms.end()){set_status(s,L"Select a Chat Room first.");return;}room=*it;s.chat.room=id;}else room.name.clear();
    door_set(s.chat.name,room.name);door_check(s.chat.enabled,room.enabled);SendMessageW(s.chat.mode,CB_SETCURSEL,static_cast<unsigned>(room.access.mode),0);door_set_number(s.chat.level,room.access.level);door_check(s.chat.sysop,room.access.sysop_only);
    s.chat.editing=true;chat_editor_enable(s,true);EnableWindow(s.chat.enabled,s.chat.room!=s.chat.config->entry);
    SetWindowTextW(s.chat.detail,add?L"Add Chat Room - Enabled, minimum security level 10.":L"Edit Chat Room - the room must be empty when you save. The entry room cannot be disabled or deleted.");remember_page(s);SetFocus(s.chat.name);
}
bool save_chat_room(State& s){
    if(!s.chat.config||!s.chat.editing)return true;
    auto next=*s.chat.config;app::ChatRoom room;room.id=s.chat.room;
    if(!room.id){for(const auto& r:next.rooms)room.id=std::max(room.id,r.id);if(room.id==65535)return false;++room.id;}
    unsigned level{};if(!door_number(s.chat.level,level,999)||!level){set_status(s,L"Security level must be 1 to 999. Your edits remain.");return false;}
    room.name=door_text(s.chat.name);room.enabled=door_checked(s.chat.enabled);room.access={static_cast<app::DoorSecurityMode>(SendMessageW(s.chat.mode,CB_GETCURSEL,0,0)),static_cast<std::uint16_t>(level),door_checked(s.chat.sysop)};
    if(s.chat.room){for(auto& r:next.rooms)if(r.id==s.chat.room)r=room;}else next.rooms.push_back(room);
    if(!chat_commit(s,next))return false;s.chat.editing=false;chat_editor_enable(s,false);remember_page(s);return true;
}
void chat_delete(State& s){
    if(!s.chat.config||!resolve_dirty(s))return;const auto id=chat_selected_room(s);if(!id)return;
    if(id==s.chat.config->entry){set_status(s,L"The entry Chat Room cannot be deleted.");return;}
    if(MessageBoxW(s.window,L"Delete this Chat Room? It must be empty. This cannot be undone.",kWindowTitle,MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    auto next=*s.chat.config;next.rooms.erase(std::remove_if(next.rooms.begin(),next.rooms.end(),[&](const auto& r){return r.id==id;}),next.rooms.end());chat_commit(s,next);
}
void chat_command_edit(State& s,bool add){
    if(!s.chat.config||!resolve_dirty(s))return;const auto catalog=load_door_catalog(s);if(!catalog){set_status(s,L"Doors are unavailable. No change was made.");return;}
    auto& c=s.chat;app::ChatDoorCommand value;c.command=-1;
    if(!add){const auto row=SendMessageW(c.commands,LB_GETCURSEL,0,0);if(row==LB_ERR)return;const auto n=SendMessageW(c.commands,LB_GETITEMDATA,row,0);if(n<0||static_cast<std::size_t>(n)>=c.config->commands.size())return;c.command=static_cast<int>(n);value=c.config->commands[n];}
    SendMessageW(c.command_door,CB_RESETCONTENT,0,0);
    for(const auto& door:catalog->doors){const auto name=utf8_to_wide(door.name);const auto row=SendMessageW(c.command_door,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(c.command_door,CB_SETITEMDATA,row,door.id);if(door.id==value.door)SendMessageW(c.command_door,CB_SETCURSEL,row,0);}
    for(int i=0;i<5;++i)if(SendMessageW(c.command_menu,CB_GETITEMDATA,i,0)==static_cast<LRESULT>(value.menu))SendMessageW(c.command_menu,CB_SETCURSEL,i,0);
    door_set(c.command_key,add?"":std::string(1,value.key));door_set(c.command_name,value.name);door_check(c.command_enabled,value.enabled);SendMessageW(c.command_mode,CB_SETCURSEL,static_cast<unsigned>(value.access.mode),0);door_set_number(c.command_level,value.access.level);door_check(c.command_sysop,value.access.sysop_only);c.command_editing=true;chat_command_enable(s,true);remember_page(s);SetFocus(c.command_door);
}
bool save_chat_command(State& s){
    auto& c=s.chat;if(!c.config||!c.command_editing)return true;app::ChatDoorCommand value;
    const auto menu=SendMessageW(c.command_menu,CB_GETCURSEL,0,0),door=SendMessageW(c.command_door,CB_GETCURSEL,0,0);unsigned level{};auto key=door_text(c.command_key);
    if(menu==CB_ERR||door==CB_ERR||key.size()!=1||!door_number(c.command_level,level,999)||!level){set_status(s,L"Choose a menu, Door, unused letter and security level.");return false;}
    value.menu=static_cast<app::MenuId>(SendMessageW(c.command_menu,CB_GETITEMDATA,menu,0));value.door=static_cast<app::DoorId>(SendMessageW(c.command_door,CB_GETITEMDATA,door,0));value.key=static_cast<char>(key[0]>='a'&&key[0]<='z'?key[0]-'a'+'A':key[0]);value.name=door_text(c.command_name);value.enabled=door_checked(c.command_enabled);value.access={static_cast<app::DoorSecurityMode>(SendMessageW(c.command_mode,CB_GETCURSEL,0,0)),static_cast<std::uint16_t>(level),door_checked(c.command_sysop)};
    auto next=*c.config;if(c.command<0)next.commands.push_back(value);else next.commands[c.command]=value;
    if(!chat_commit(s,next,ChatSaveTask::commands))return false;c.command_editing=false;chat_command_enable(s,false);remember_page(s);return true;
}
bool chat_configuration_command(State& s,int id,int notification){
    switch(id){
    case IDC_CHAT_ADD:chat_edit(s,true);return true;
    case IDC_CHAT_EDIT:chat_edit(s,false);return true;
    case IDC_CHAT_DELETE:chat_delete(s);return true;
    case IDC_CHAT_RELOAD:case IDC_CHAT_CANCEL:if(resolve_dirty(s)){refresh_chat(s);remember_page(s);}return true;
    case IDC_CHAT_SAVE:save_chat_room(s);return true;
    case IDC_CHAT_STATS_SAVE:if(changed_control(s,s.opening_date)){set_status(s,L"Save or discard the opening date edit first. Your statistics edit remains.");return true;}if(s.chat.config){auto next=*s.chat.config;next.exclude_sysop_statistics=door_checked(s.chat.stats);chat_commit(s,next,ChatSaveTask::statistics);}return true;
    case IDC_CHAT_COMMANDS:if(resolve_dirty(s)){activate_page(s,28);refresh_chat(s);remember_page(s);}return true;
    case IDC_CHAT_COMMAND_BACK:if(resolve_dirty(s))activate_page(s,17);return true;
    case IDC_CHAT_COMMAND_ADD:chat_command_edit(s,true);return true;
    case IDC_CHAT_COMMAND_EDIT:chat_command_edit(s,false);return true;
    case IDC_CHAT_COMMAND_SAVE:save_chat_command(s);return true;
    case IDC_CHAT_COMMAND_DOOR:if(notification==CBN_SELCHANGE&&s.chat.command_editing&&text(s.chat.command_name).empty()){const auto row=SendMessageW(s.chat.command_door,CB_GETCURSEL,0,0);wchar_t name[128]{};if(row!=CB_ERR&&SendMessageW(s.chat.command_door,CB_GETLBTEXTLEN,row,0)<128){SendMessageW(s.chat.command_door,CB_GETLBTEXT,row,reinterpret_cast<LPARAM>(name));SetWindowTextW(s.chat.command_name,name);}}return true;
    case IDC_CHAT_COMMAND_DELETE:if(s.chat.config&&resolve_dirty(s)){const auto row=SendMessageW(s.chat.commands,LB_GETCURSEL,0,0);if(row!=LB_ERR){const auto n=SendMessageW(s.chat.commands,LB_GETITEMDATA,row,0);auto next=*s.chat.config;if(n>=0&&static_cast<std::size_t>(n)<next.commands.size()){next.commands.erase(next.commands.begin()+n);chat_commit(s,next,ChatSaveTask::commands);}}}return true;
    default:return false;
    }
}
void create_chat_pages(State& s){
    auto& c=s.chat;const auto p=s.pages[27]=page(s);
    label(s,p,L"Chat Rooms",12,8,500,28);label(s,p,L"Choose a named room. Rooms must be empty before changes can be saved.",12,43,500,38);
    c.list=control(s,p,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,12,90,494,180,IDC_CHAT_LIST,WS_EX_CLIENTEDGE);
    const auto button=[&](HWND parent,const wchar_t* name,int x,int y,int width,int id){return control(s,parent,L"BUTTON",name,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,30,id);};
    button(p,L"Add Room",12,286,150,IDC_CHAT_ADD);button(p,L"Edit Room",176,286,150,IDC_CHAT_EDIT);button(p,L"Delete Room",340,286,166,IDC_CHAT_DELETE);
    c.detail=control(s,p,L"STATIC",L"",SS_NOPREFIX,12,330,494,44,0);
    label(s,p,L"Room name",12,383,494,20);c.name=control(s,p,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,12,405,494,25,IDC_CHAT_NAME);SendMessageW(c.name,EM_SETLIMITTEXT,64,0);
    c.enabled=control(s,p,L"BUTTON",L"Enabled",WS_TABSTOP|BS_AUTOCHECKBOX,12,442,494,25,IDC_CHAT_ENABLED);
    const auto requirement=[&](HWND parent,int y,int modeid,int levelid,int sysopid,HWND& mode,HWND& level,HWND& sysop){
        label(s,parent,L"Access Requirements",12,y,250,20);label(s,parent,L"Security level (1-999)",278,y,228,20);
        mode=control(s,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,12,y+23,250,120,modeid);for(auto name:{L"At least this security level",L"At most this security level",L"Exactly this security level"})SendMessageW(mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
        level=control(s,parent,L"EDIT",L"10",WS_TABSTOP|WS_BORDER|ES_NUMBER,278,y+23,228,25,levelid);SendMessageW(level,EM_SETLIMITTEXT,3,0);
        sysop=control(s,parent,L"BUTTON",L"Sysop Only - uses the BBS Sysop security level",WS_TABSTOP|BS_AUTOCHECKBOX,12,y+57,494,25,sysopid);
    };
    requirement(p,480,IDC_CHAT_MODE,IDC_CHAT_LEVEL,IDC_CHAT_SYSOP,c.mode,c.level,c.sysop);
    button(p,L"Save Changes",12,590,238,IDC_CHAT_SAVE);button(p,L"Cancel / Reload",268,590,238,IDC_CHAT_CANCEL);chat_editor_enable(s,false);
    c.stats=control(s,s.pages[18],L"BUTTON",L"Exclude Sysop from Statistics",WS_TABSTOP|BS_AUTOCHECKBOX,12,310,494,25,IDC_CHAT_STATS);
    label(s,s.pages[18],L"Exclude future Sysop logons, system time online and recent-caller summaries. Personal account history and existing totals are preserved.",12,346,494,55);
    button(s.pages[18],L"Save Statistics Setting",12,414,250,IDC_CHAT_STATS_SAVE);
    const auto q=s.pages[28]=page(s);label(s,q,L"Door Commands on Primary Menus",12,8,494,28);
    label(s,q,L"Choose a registered Door by name. Shared Door access and execution requirements still apply.",12,44,494,42);
    c.commands=control(s,q,L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,12,96,494,130,IDC_CHAT_COMMAND_LIST,WS_EX_CLIENTEDGE);
    button(q,L"Add Command",12,240,150,IDC_CHAT_COMMAND_ADD);button(q,L"Edit Command",176,240,150,IDC_CHAT_COMMAND_EDIT);button(q,L"Remove Command",340,240,166,IDC_CHAT_COMMAND_DELETE);
    label(s,q,L"Menu",12,286,200,20);label(s,q,L"Registered Door",224,286,282,20);
    c.command_menu=control(s,q,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,12,310,200,150,IDC_CHAT_COMMAND_MENU);
    for(const auto id:{app::MenuId::main,app::MenuId::messages,app::MenuId::files,app::MenuId::chat,app::MenuId::doors}){const auto name=utf8_to_wide(app::menu_definition(id).title);const auto row=SendMessageW(c.command_menu,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(c.command_menu,CB_SETITEMDATA,row,static_cast<LPARAM>(id));}
    c.command_door=control(s,q,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,224,310,282,200,IDC_CHAT_COMMAND_DOOR);
    label(s,q,L"Unused letter (A-Z)",12,350,165,20);label(s,q,L"Name on this menu",180,350,326,20);
    c.command_key=control(s,q,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_UPPERCASE,12,374,150,25,IDC_CHAT_COMMAND_KEY);SendMessageW(c.command_key,EM_SETLIMITTEXT,1,0);
    c.command_name=control(s,q,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,180,374,326,25,IDC_CHAT_COMMAND_NAME);SendMessageW(c.command_name,EM_SETLIMITTEXT,64,0);
    c.command_enabled=control(s,q,L"BUTTON",L"Enabled on this menu",WS_TABSTOP|BS_AUTOCHECKBOX,12,412,494,25,IDC_CHAT_COMMAND_ENABLED);
    requirement(q,455,IDC_CHAT_COMMAND_MODE,IDC_CHAT_COMMAND_LEVEL,IDC_CHAT_COMMAND_SYSOP,c.command_mode,c.command_level,c.command_sysop);
    label(s,q,L"This is an additional restriction. Minimum 1 adds no restriction beyond the Door's shared requirements.",12,546,494,42);
    button(q,L"Save Changes",12,608,238,IDC_CHAT_COMMAND_SAVE);button(q,L"Back to Displays & Menus",268,608,238,IDC_CHAT_COMMAND_BACK);
}
