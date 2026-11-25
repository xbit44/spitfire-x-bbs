void ftn_echo_fill(State& s,bool list_only=false){
    auto& f=s.ftn;const auto* c=ftn_current(s);if(!c||f.network<0||static_cast<std::size_t>(f.network)>=c->networks.size())return;
    f.populating=true;const auto& n=c->networks[f.network];SendMessageW(f.echoes,LB_RESETCONTENT,0,0);const auto find=ftn_fold(door_text(f.echo_find));
    for(std::size_t i=0;i<c->message_echoes.size();++i){const auto& e=c->message_echoes[i];if(e.network!=n.code||(!find.empty()&&ftn_fold(e.tag+" "+e.description).find(find)==std::string::npos))continue;const auto title=utf8_to_wide(e.tag+(e.store_locally?"  -  Local conference":"  -  Pass through"));const auto row=SendMessageW(f.echoes,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));SendMessageW(f.echoes,LB_SETITEMDATA,row,i);if(static_cast<int>(i)==f.echo)SendMessageW(f.echoes,LB_SETCURSEL,row,0);}
    if(list_only){f.populating=false;return;}
    const bool valid=f.echo>=0&&static_cast<std::size_t>(f.echo)<c->message_echoes.size()&&c->message_echoes[f.echo].network==n.code;
    for(const auto h:{f.echo_tag,f.echo_description,f.echo_name,f.echo_store,f.echo_group,f.echo_local,f.echo_access,f.echo_read,f.echo_post,f.echo_real_name,f.echo_pipe,f.echo_ansi,f.echo_count,f.echo_age})EnableWindow(h,valid&&f.catalog_ready);
    ftn_group_choices(s,f.echo_group,n.message_group);SendMessageW(f.echo_local,CB_RESETCONTENT,0,0);ftn_combo_item(f.echo_local,L"Create a new local conference",0);
    for(const auto& e:f.local_echoes)ftn_combo_item(f.echo_local,std::to_wstring(e.conference_number)+L"  "+utf8_to_wide(e.name),e.id.value);
    if(valid){const auto& e=c->message_echoes[f.echo];door_set(f.echo_tag,e.tag);door_set(f.echo_description,e.description);door_set(f.echo_name,e.local_name.empty()?e.tag:e.local_name);door_check(f.echo_store,e.store_locally);door_check(f.echo_real_name,e.require_real_name);door_check(f.echo_pipe,e.pipe_colors);SendMessageW(f.echo_ansi,CB_SETCURSEL,e.ansi_colors?(*e.ansi_colors?2:1):0,0);door_set(f.echo_count,e.maximum_messages?std::to_string(*e.maximum_messages):"Unlimited");door_set(f.echo_age,e.maximum_age_days?std::to_string(*e.maximum_age_days):"0");door_set_number(f.echo_access,e.access_level);door_set_number(f.echo_read,e.read_level);door_set_number(f.echo_post,e.post_level);ftn_combo_select(f.echo_local,e.local_echo);}
    EnableWindow(GetDlgItem(s.pages[32],IDC_FTN_ECHO_SAVE),valid&&f.catalog_ready);EnableWindow(GetDlgItem(s.pages[32],IDC_FTN_ECHO_REMOVE),valid);f.populating=false;
}
bool collect_ftn_echo(State& s,app::FtnConfiguration& c){
    auto& f=s.ftn;if(f.echo<0)return true;if(!f.catalog_ready||static_cast<std::size_t>(f.echo)>=c.message_echoes.size()||f.network<0||c.message_echoes[f.echo].network!=c.networks[f.network].code)return false;
    auto& e=c.message_echoes[f.echo];unsigned access{},read{},post{};const auto group=ftn_combo_value(f.echo_group),local=ftn_combo_value(f.echo_local);
    if(!door_number(f.echo_access,access,999)||!access||!door_number(f.echo_read,read,999)||!read||!door_number(f.echo_post,post,999)||!post||group<0||local<0){set_status(s,L"Choose a message group/conference and Access, Read and Post levels from 1 to 999.");return false;}
    const auto tag=ftn_fold(door_text(f.echo_tag));if(e.local_echo&&tag!=e.tag){set_status(s,L"A saved echo tag is fixed. Add another echo to use a different tag; existing messages remain.");return false;}
    e.tag=tag;e.description=door_text(f.echo_description);e.store_locally=door_checked(f.echo_store);e.local_name=door_text(f.echo_name);e.local_echo=e.store_locally?static_cast<std::int64_t>(local):0;e.require_real_name=door_checked(f.echo_real_name);
    e.update_local_permissions=e.local_echo&&(e.access_level!=access||e.read_level!=read||e.post_level!=post||e.pipe_colors!=door_checked(f.echo_pipe));e.update_local_name=false;
    if(e.local_echo){const auto mapped=std::find_if(f.local_echoes.begin(),f.local_echoes.end(),[&](const auto& v){return v.id.value==e.local_echo;});if(mapped==f.local_echoes.end())return false;e.update_local_name=e.local_name!=mapped->name;}
    const auto limit=[&](HWND field,std::uint32_t maximum,bool age,std::optional<std::uint32_t>& value){const auto text=ftn_fold(door_text(field));if(text.empty()||text=="UNLIMITED"||(age&&text=="0")){value.reset();return true;}unsigned n{};if(!door_number(field,n,maximum)||!n)return false;value=n;return true;};
    std::optional<std::uint32_t> count,age;if(!limit(f.echo_count,app::kMaximumRetainedMessageCount,false,count)||!limit(f.echo_age,app::kMaximumMessageAgeDays,true,age)){set_status(s,L"Enter valid message limits or Unlimited. Your edits remain.");return false;}if(e.maximum_messages!=count){e.maximum_messages=count;e.update_local_retention|=1;}if(e.maximum_age_days!=age){e.maximum_age_days=age;e.update_local_retention|=2;}const auto ansi=SendMessageW(f.echo_ansi,CB_GETCURSEL,0,0);e.ansi_colors=ansi==0?std::optional<bool>{}:std::optional<bool>{ansi==2};
    e.access_level=static_cast<std::uint16_t>(access);e.read_level=static_cast<std::uint16_t>(read);e.post_level=static_cast<std::uint16_t>(post);e.pipe_colors=door_checked(f.echo_pipe);
    if(e.store_locally&&!e.local_echo)c.networks[f.network].message_group=static_cast<std::int64_t>(group);if(f.config)app::link_new_ftn_echoes_to_hub(*f.config,c,e.network);return true;
}
void ftn_echo_edit(State& s,bool add){
    auto& f=s.ftn;auto c=*ftn_current(s);if(!collect_ftn_echo(s,c)){ftn_list_select(f.echoes,f.echo);return;}
    if(add){app::FtnMessageEcho e;e.network=c.networks[f.network].code;e.tag="NEW.ECHO";e.ansi_colors=false;e.require_real_name=ftn_fold(c.networks[f.network].name)=="FIDONET";f.echo=static_cast<int>(c.message_echoes.size());c.message_echoes.push_back(std::move(e));f.draft_changed=true;}
    else{const auto row=SendMessageW(f.echoes,LB_GETCURSEL,0,0);if(row==LB_ERR)return;f.echo=static_cast<int>(SendMessageW(f.echoes,LB_GETITEMDATA,row,0));}
    f.draft=std::move(c);ftn_echo_fill(s);remember_page(s);SetFocus(f.echo_tag);
}
void create_ftn_echoes(State& s){
    auto& f=s.ftn;FtnControls ui{s,s.pages[32]=page(s)};ui.label(L"Message Echoes",12,8);f.echo_find=ui.edit(L"Find by echo tag or description",12,40,510,IDC_FTN_ECHO_FIND,128);f.echoes=ui.list(12,96,510,115,IDC_FTN_ECHO_LIST);
    ui.button(L"Add Echo",12,223,150,IDC_FTN_ECHO_ADD);ui.button(L"Edit Echo",174,223,150,IDC_FTN_ECHO_EDIT);ui.button(L"Import Echo List...",336,223,186,IDC_FTN_ECHO_IMPORT);
    f.echo_tag=ui.edit(L"Echo tag",12,267,240,IDC_FTN_ECHO_TAG,128);f.echo_description=ui.edit(L"Description",270,267,252,IDC_FTN_ECHO_DESCRIPTION,512);
    f.echo_store=ui.check(L"Store messages in a local conference (unchecked = pass through)",12,329,510,IDC_FTN_ECHO_STORE);
    f.echo_group=ui.combo(L"Message group for a new conference",12,369,240,IDC_FTN_ECHO_GROUP);f.echo_local=ui.combo(L"Local conference",270,369,252,IDC_FTN_ECHO_LOCAL);
    f.echo_name=ui.edit(L"Local conference name (64 characters maximum)",12,431,510,IDC_FTN_ECHO_LOCAL_NAME,64);
    f.echo_access=ui.edit(L"Access level",12,493,156,IDC_FTN_ECHO_ACCESS,3);f.echo_read=ui.edit(L"Read level",186,493,156,IDC_FTN_ECHO_READ,3);f.echo_post=ui.edit(L"Post level",360,493,162,IDC_FTN_ECHO_POST,3);
    f.echo_real_name=ui.check(L"Require Real Name on outgoing messages",12,553,510,IDC_FTN_ECHO_REAL_NAME);f.echo_pipe=ui.check(L"Enable pipe colors for this public echo",12,587,510,IDC_FTN_ECHO_PIPE);
    f.echo_ansi=ui.combo(L"Allow ANSI in outgoing posts",12,625,510,IDC_FTN_ECHO_ANSI);for(const auto* text:{L"Keep existing outgoing policy",L"Off",L"On"})SendMessageW(f.echo_ansi,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    f.echo_count=ui.edit(L"Maximum messages (Unlimited allowed)",12,687,252,IDC_FTN_ECHO_COUNT,12);f.echo_age=ui.edit(L"Maximum age, days (0 = Unlimited)",282,687,240,IDC_FTN_ECHO_AGE,12);ui.button(L"Clone Options...",12,752,180,IDC_FTN_ECHO_CLONE);
    ui.label(L"Message group restrictions also apply. Saving limits does not delete messages.",12,802,510,40);
    ui.button(L"Save Changes",12,852,156,IDC_FTN_ECHO_SAVE);ui.button(L"Remove Mapping...",180,852,174,IDC_FTN_ECHO_REMOVE);ui.button(L"Back to Network",366,852,156,IDC_FTN_ECHO_BACK);
}
