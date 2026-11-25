unsigned door_command_identity(const app::ChatDoorCommand& command){return static_cast<unsigned>(command.menu)*26U+static_cast<unsigned>(command.key-'A')+1U;}
namespace {
bool command_menu(unsigned menu){return menu==0||menu==1||menu==2||menu==5||menu==8;}
bool command_identity(unsigned id){return id&&id<=234&&command_menu((id-1U)/26U);}
Json command_fields(app::ChatDoorCommand& command,const app::DoorCatalog& catalog,const Json* input=nullptr){
    Fields f;f.input=input;auto menu=static_cast<unsigned>(command.menu);f.integer("menu","Menu",menu,0,8);require(command_menu(menu),"command_menu_invalid");command.menu=static_cast<app::MenuId>(menu);
    f.items.back()["type"]="select";f.items.back()["choices"]=Json::array();for(const auto id:{0U,1U,2U,8U,5U})f.items.back()["choices"].push_back({{"value",id},{"label",app::menu_definition(static_cast<app::MenuId>(id)).title}});
    f.integer("door","Registered Door",command.door,1,999);f.items.back()["type"]="select";f.items.back()["choices"]=Json::array();
    for(const auto& door:catalog.doors)f.items.back()["choices"].push_back({{"value",door.id},{"label",door.name+(door.enabled?"":" (disabled)")}});
    if(!command.door)f.items.back()["choices"].push_back({{"value",0},{"label","Choose a registered Door"}});
    else if(std::none_of(catalog.doors.begin(),catalog.doors.end(),[&](const auto& door){return door.id==command.door;}))f.items.back()["choices"].push_back({{"value",command.door},{"label","Unavailable Door"}});
    std::string key(1,command.key);f.text("key","Unused letter (A-Z)",key,1);if(input){require(key.size()==1&&key[0]>='A'&&key[0]<='Z',"command_key_invalid");command.key=key[0];}
    f.text("name","Name on this menu",command.name,64);f.boolean("enabled","Enabled on this menu",command.enabled);
    auto mode=static_cast<unsigned>(command.access.mode);f.integer("security_mode","Additional security level rule",mode,0,2,"Shared Door access and execution requirements still apply.");command.access.mode=static_cast<app::DoorSecurityMode>(mode);
    f.items.back()["type"]="select";f.items.back()["choices"]=Json::array({{{"value",0},{"label","Minimum level"}},{{"value",1},{"label","Maximum level"}},{{"value",2},{"label","Exact level"}}});
    f.integer("security_level","Additional security level",command.access.level,1,999,"Minimum 1 adds no restriction beyond the Door's shared requirements.");f.boolean("sysop_only","Sysop only",command.access.sysop_only);f.finish();return f.items;
}
}
Json door_command_page(const app::ChatConfiguration& current,const app::DoorCatalog& catalog,std::optional<unsigned> requested){
    require(app::valid_chat_configuration(current),"command_configuration_unavailable");const unsigned selected=requested.value_or(current.commands.empty()?0:door_command_identity(current.commands.front()));
    require(!selected||command_identity(selected),"command_identity_invalid");app::ChatDoorCommand command;bool missing=false;
    if(selected){const auto found=std::find_if(current.commands.begin(),current.commands.end(),[&](const auto& value){return door_command_identity(value)==selected;});if(found==current.commands.end())missing=true;else command=*found;}
    if(!selected){for(char key='A';key<='Z';++key)if(app::available_door_command_key(command.menu,key)&&std::none_of(current.commands.begin(),current.commands.end(),[&](const auto& value){return value.menu==command.menu&&value.key==key;})){command.key=key;break;}}
    const auto fields=command_fields(command,catalog);Json records=Json::array();for(const auto& value:current.commands)records.push_back({{"id",door_command_identity(value)},{"name",std::string(app::menu_definition(value.menu).title)+" ["+value.key+"] "+value.name+(value.enabled?"":" (disabled)")}});
    if(missing)records.push_back({{"id",selected},{"name","Removed command (draft retained)"}});
    return {{"route","general.door_commands"},{"title",selected?"Door Commands":"Add Door Command"},{"category","General"},{"revision",std::to_string(current.revision)},
        {"activation",missing?"This command was removed. Discard the draft to select another command; saving cannot recreate it.":"Commands share saved settings with Chat Rooms and Statistics. Shared Door access and execution requirements still apply."},
        {"record",selected},{"records",records},{"can_save",!missing&&!catalog.doors.empty()},{"can_remove",selected&&!missing},{"fields",fields},{"active_fields",fields}};
}
app::ChatConfiguration door_command_update(const Json& request,const app::ChatConfiguration& current,const app::DoorCatalog& catalog,bool remove){
    require(request.is_object()&&request.size()==(remove?2U:3U)&&request.contains("revision")&&request.contains("record")&&(remove||request.contains("values")),"command_request_invalid");
    const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");const auto selected=bounded_number(request.at("record"),0,234);require(!selected||command_identity(static_cast<unsigned>(selected)),"command_identity_invalid");
    auto next=current;next.revision=static_cast<std::int64_t>(expected);auto found=std::find_if(next.commands.begin(),next.commands.end(),[&](const auto& value){return door_command_identity(value)==selected;});
    if(remove){require(selected&&found!=next.commands.end(),"command_not_found");next.commands.erase(found);}
    else{app::ChatDoorCommand command;if(selected){require(found!=next.commands.end(),"command_not_found");command=*found;}else require(next.commands.size()<130,"command_limit_reached");
        (void)command_fields(command,catalog,&request.at("values"));require(std::any_of(catalog.doors.begin(),catalog.doors.end(),[&](const auto& door){return door.id==command.door;}),"command_door_unavailable");
        if(selected)*found=command;else next.commands.push_back(command);
    }
    require(app::valid_chat_configuration(next),"command_configuration_invalid");return next;
}
