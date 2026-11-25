namespace {
constexpr std::uint16_t security_defaults_record=1000;
void security_fields(Fields& fields,app::LocalSecurityChange& change,bool defaults){
    if(defaults){
        fields.integer("new_user_level","New caller security level",change.new_user_level,1,999);
        fields.integer("sysop_threshold","Sysop security threshold",change.sysop_threshold,1,999,"Every caller at or above this level has Sysop authority. New Sysop access requires a fresh login and SY authentication.");
        return;
    }
    fields.integer("level","Level number",change.level,1,999);
    fields.text("label","Label",change.label,app::kSecurityLabelMaximum);
    auto description=change.description.value_or("");fields.text("description","Description",description,app::kSecurityDescriptionMaximum);
    change.description=description.empty()?std::optional<std::string>{}:description;
    const auto minutes=[&](const char* key,const char* label,std::optional<std::uint32_t>& value){
        auto amount=value.value_or(0);fields.integer(key,label,amount,0,UINT32_MAX,"0 means unlimited.");value=amount?std::optional<std::uint32_t>{amount}:std::nullopt;
    };
    minutes("call_minutes","Minutes per call",change.minutes_per_call);
    minutes("day_minutes","Minutes per day",change.minutes_per_day);
    minutes("chat_minutes","Private chat minutes per call",change.private_chat_minutes);
    minutes("chat_day_minutes","Private chat minutes per day",change.private_chat_minutes_per_day);
}
std::uint16_t security_record(const Json& request){
    require(request.contains("record")&&request.at("record").is_number_integer()&&request.at("record")>=Json(0)&&request.at("record")<=Json(security_defaults_record),"security_record_invalid");
    return request.at("record").get<std::uint16_t>();
}
app::LocalSecurityChange security_baseline(const Json& request,const app::LocalSecuritySnapshot& current){
    const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");
    require(current.code==app::LocalAccountResultCode::ok&&!current.has_more&&expected==static_cast<std::uint64_t>(current.policy.revision),"security_revision_changed");
    app::LocalSecurityChange change;change.expected_revision=static_cast<std::int64_t>(expected);change.new_user_level=current.policy.new_user_level;change.sysop_threshold=current.policy.sysop_threshold;return change;
}
}
Json security_level_page(const app::LocalSecuritySnapshot& current,std::optional<std::uint16_t> selection){
    require(current.code==app::LocalAccountResultCode::ok&&!current.has_more&&current.policy.revision>0,"security_unavailable");
    const auto selected=selection.value_or(current.levels.empty()?0:current.levels.front().level);require(selected<=security_defaults_record,"security_record_invalid");
    const bool defaults=selected==security_defaults_record;const auto found=std::find_if(current.levels.begin(),current.levels.end(),[&](const auto& value){return value.level==selected;});
    const bool existing=found!=current.levels.end(),active=existing&&found->state==app::SecurityLevelState::active,available=!selected||defaults||active;
    app::LocalSecurityChange change;change.new_user_level=current.policy.new_user_level;change.sysop_threshold=current.policy.sysop_threshold;
    if(existing){change.level=selected;change.label=found->label;change.description=found->description;change.minutes_per_call=found->minutes_per_call;change.minutes_per_day=found->minutes_per_day;change.private_chat_minutes=found->private_chat_minutes;change.private_chat_minutes_per_day=found->private_chat_minutes_per_day;}
    else change.level=selected?selected:1;
    Fields fields;security_fields(fields,change,defaults);
    Json records=Json::array({{{"id",security_defaults_record},{"name","New caller level and Sysop threshold"}}});
    for(const auto& level:current.levels)records.push_back({{"id",level.level},{"name",std::to_string(level.level)+"  "+level.label+(level.state==app::SecurityLevelState::retired?" (removed)":"")}});
    if(selected&&!defaults&&!existing)records.push_back({{"id",selected},{"name",std::to_string(selected)+"  Unavailable level (draft retained)"}});
    for(auto& field:fields.items){field["readonly"]=!available||(!defaults&&selected&&field["key"]=="level")||(!defaults&&selected==999&&(field["key"]=="call_minutes"||field["key"]=="day_minutes"));}
    const std::string detail=!available?"This level is removed or unavailable. Its number cannot be reused.":defaults?"Defaults save separately from level definitions. Choose active existing levels.":"Saved reductions apply to online callers. Only level 999 has unlimited calling time, no AFK timeout and simultaneous sessions. 0 minutes means unlimited.";
    return {{"route","users.security_levels"},{"title","Security Levels"},{"category","Users & Access"},{"revision",std::to_string(current.policy.revision)},{"record",selected},{"records",records},{"can_save",available},{"can_remove",active},{"fields",fields.items},{"active_fields",fields.items},{"activation",detail}};
}
app::LocalSecurityChange security_level_update(const Json& request,const app::LocalSecuritySnapshot& current){
    require(request.is_object()&&request.size()==3&&request.contains("revision")&&request.contains("values"),"security_request_invalid");
    const auto selected=security_record(request);auto change=security_baseline(request,current);const bool defaults=selected==security_defaults_record;
    change.action=defaults?app::LocalSecurityAction::set_defaults:app::LocalSecurityAction::upsert_level;
    Fields fields;fields.input=&request.at("values");security_fields(fields,change,defaults);fields.finish();
    if(!defaults){
        const auto found=std::find_if(current.levels.begin(),current.levels.end(),[&](const auto& value){return value.level==(selected?selected:change.level);});
        require(selected?(change.level==selected&&found!=current.levels.end()&&found->state==app::SecurityLevelState::active):found==current.levels.end(),"security_selection_changed");
    }
    require(bool(app::prepare_local_security_change(current,change)),"security_values_invalid");return change;
}
app::LocalSecurityChange security_level_removal(const Json& request,const app::LocalSecuritySnapshot& current){
    require(request.is_object()&&request.size()==4&&request.contains("revision")&&request.contains("replacement")&&request.contains("affected_hash"),"security_removal_invalid");
    auto change=security_baseline(request,current);change.action=app::LocalSecurityAction::retire_level;change.level=security_record(request);require(change.level&&change.level<security_defaults_record,"security_record_invalid");
    require(request.at("replacement").is_number_integer()&&request.at("replacement")>=Json(1)&&request.at("replacement")<=Json(999),"security_replacement_invalid");change.replacement=request.at("replacement").get<std::uint16_t>();
    require(request.at("affected_hash").is_string(),"security_review_required");change.affected_accounts_hash=request.at("affected_hash").get<std::string>();
    require(change.affected_accounts_hash.size()==64&&std::all_of(change.affected_accounts_hash.begin(),change.affected_accounts_hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"security_review_required");
    if(change.new_user_level==change.level)change.new_user_level=change.replacement;
    if(change.sysop_threshold==change.level)change.sysop_threshold=change.replacement;
    require(bool(app::prepare_local_security_change(current,change)),"security_removal_invalid");return change;
}
app::MessageConfigurationChange message_utility_update(std::string_view kind,const Json& request){
    const bool sysop=kind=="system_sysop",lines=kind=="line_limit",colors=kind=="private_colors",age=kind=="unread_age";
    require(sysop||lines||colors||age,"message_utility_invalid");
    require(request.is_object()&&request.size()==(sysop?8U:7U)&&request.contains("kind")&&request.at("kind")==kind&&request.contains("revision")&&request.contains("operation")&&request.contains("record")&&request.contains("record_revision")&&request.contains("value")&&request.contains("policy")&&request.at("policy")=="message"&&(!sysop||request.contains("assignment_revision")),"message_utility_request_invalid");
    app::MessageConfigurationChange change;change.operation=operation_id(request.at("operation"));
    const auto expected=revision(request.at("revision")),entity=revision(request.at("record_revision"));
    require(expected<static_cast<std::uint64_t>(INT64_MAX)&&entity<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");change.expected_configuration_revision=static_cast<std::int64_t>(expected);
    if(lines){
        change.action=app::MessageConfigurationAction::line_limit;
        require(request.at("record").is_number_integer()&&request.at("record")>=Json(1)&&request.at("record")<=Json(999),"message_level_invalid");change.security_level=request.at("record").get<std::uint16_t>();
        require(request.at("value").is_number_integer()&&request.at("value")>=Json(1)&&request.at("value")<=Json(65535),"message_lines_invalid");change.maximum_lines=request.at("value").get<std::uint32_t>();change.expected_limit_revision=static_cast<std::int64_t>(entity);
    }else if(age){
        require(request.at("record").is_null(),"unread_age_invalid");change.action=app::MessageConfigurationAction::unread_mail_retention;change.expected_unread_retention_revision=static_cast<std::int64_t>(entity);
        change.unread_mail_age_days=request.at("value").is_null()?std::nullopt:std::optional(static_cast<std::uint32_t>(bounded_number(request.at("value"),1,65535)));
    }else if(colors){
        require(request.at("record").is_null()&&request.at("value").is_boolean(),"message_colors_invalid");change.action=app::MessageConfigurationAction::private_presentation;change.private_pipe_colors=request.at("value").get<bool>();change.expected_private_presentation_revision=static_cast<std::int64_t>(entity);
    }else{
        require(request.at("value")==request.at("record"),"sysop_selection_changed");const auto account=revision(request.at("record")),assignment=revision(request.at("assignment_revision"));
        require(account<=static_cast<std::uint64_t>(INT64_MAX)&&assignment<static_cast<std::uint64_t>(INT64_MAX),"sysop_revision_invalid");change.action=app::MessageConfigurationAction::system_sysop;change.sysop=static_cast<app::AccountId>(account);change.expected_account_revision=static_cast<std::int64_t>(entity);change.expected_assignment_revision=static_cast<std::int64_t>(assignment);
    }
    require(app::valid_message_configuration_change(change),"message_utility_invalid");return change;
}
