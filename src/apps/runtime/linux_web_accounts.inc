namespace {
void account_fields(Fields& fields,app::LocalAccountRequest& value){
    fields.text("name","Name",value.name,app::kAccountNameMaximum);
    const auto optional=[&](const char* key,const char* label,std::optional<std::string>& item,std::size_t maximum){auto text=item.value_or("");fields.text(key,label,text,maximum);item=text.empty()?std::optional<std::string>{}:text;};
    optional("real_name","Real name",value.profile.real_name,app::kRealNameMaximum);optional("location","Location",value.profile.location,app::kLocationMaximum);optional("phone","Phone",value.profile.phone,app::kPhoneMaximum);optional("email","Email",value.profile.email,app::kEmailMaximum);optional("birth_date","Birth date (YYYY-MM-DD)",value.profile.birth_date,10);
    std::string sex=value.profile.sex?std::string(1,*value.profile.sex):"";fields.text("sex","Sex (M/F/X or blank)",sex,1);value.profile.sex=sex.empty()?std::optional<char>{}:sex.front();
    fields.integer("security_level","Assigned security level",value.security_level,1,999,"Shared definitions and defaults are maintained under Security Levels.");fields.boolean("purge_allowed","Allow automatic purge",value.purge_allowed);
}
app::AccountId account_record(const Json& request,bool zero){
    require(request.contains("record")&&request.at("record").is_string(),"account_selection_invalid");
    if(zero&&request.at("record")=="0")return 0;
    const auto selected=revision(request.at("record"));require(selected<=static_cast<std::uint64_t>(INT64_MAX),"account_selection_invalid");return static_cast<app::AccountId>(selected);
}
std::int64_t account_revision(const Json& value){const auto parsed=revision(value);require(parsed<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");return static_cast<std::int64_t>(parsed);}
void account_password(const Json& request,app::LocalAccountRequest& result){
    require(request.contains("password")&&request.contains("confirmation")&&request.at("password").is_string()&&request.at("confirmation").is_string(),"password_required");
    const auto& first=request.at("password").get_ref<const std::string&>();const auto& second=request.at("confirmation").get_ref<const std::string&>();
    require(!first.empty()&&first.size()<=app::kPasswordMaximum&&first==second,"password_confirmation_invalid");result.password=first;result.confirmation=second;
}
}
Json account_page(const app::LocalAccountSnapshot& current,std::optional<app::AccountId> selected){
    const bool creating=selected&&*selected==0;const app::AccountRecord* account=nullptr;
    if(selected&&!creating&&current.code==app::LocalAccountResultCode::ok&&current.accounts.size()==1&&current.accounts.front().account_id==*selected)account=&current.accounts.front();
    const bool available=creating||(account&&account->state!=app::AccountState::tombstoned);app::LocalAccountRequest value;
    value.security_level=current.policy.new_user_level?current.policy.new_user_level:10;
    if(account){value.account_id=account->account_id;value.name=account->display_name.value_or("");value.profile=account->profile;value.security_level=account->security_level.value_or(1);value.purge_allowed=account->purge_allowed.value_or(false);}
    Fields fields;account_fields(fields,value);for(auto& field:fields.items)field["readonly"]=!available||(creating&&(field["key"]=="security_level"||field["key"]=="purge_allowed"));
    Json detail={{"state",account?std::string(app::storage_code(account->state)):creating?"new":"unselected"},{"online",account&&current.online},{"temporary",nullptr}};
    if(account){detail["created_utc"]=std::to_string(account->created_utc_seconds);detail["modified_utc"]=std::to_string(account->modified_utc_seconds);detail["successful_logins"]=std::to_string(account->statistics.successful_login_count);detail["session_seconds"]=std::to_string(account->statistics.authenticated_session_seconds);}
    if(account&&current.temporary)detail["temporary"]={{"state",std::string(app::storage_code(current.temporary->state))},{"expires_utc",std::to_string(current.temporary->expires_utc_seconds)}};
    return {{"route","users.accounts"},{"title","Accounts"},{"category","Users & Access"},{"revision",account?std::to_string(account->revision):"1"},{"policy_revision",std::to_string(current.policy.revision)},{"record",selected?Json(std::to_string(*selected)):Json(nullptr)},{"can_save",available},{"can_remove",account&&available},{"fields",fields.items},{"active_fields",fields.items},{"account",detail},{"activation",creating?"New User: enter a name and initial password twice. The configured New User level is assigned when saved.":available?"Edit this caller's record, then Save. Password reset is a separate action.":selected?"This caller is deleted or unavailable. The selection is retained; saving cannot recreate it.":"Find and select a caller, or choose New User."}};
}
app::LocalAccountRequest account_update(const Json& request){
    require(request.is_object()&&request.contains("revision")&&request.contains("policy_revision")&&request.contains("values"),"account_request_invalid");
    app::LocalAccountRequest result;result.account_id=account_record(request,true);const bool creating=!result.account_id;
    require(request.size()==(creating?6U:4U),"account_request_invalid");result.action=creating?app::LocalAccountAction::create_user:app::LocalAccountAction::save_user;result.expected_revision=account_revision(request.at("revision"));result.expected_policy_revision=account_revision(request.at("policy_revision"));
    Fields fields;fields.input=&request.at("values");account_fields(fields,result);fields.finish();
    require(app::normalize_account_name(result.name)==result.name&&app::valid_profile(result.profile),"account_profile_invalid");
    if(creating){require(result.expected_revision==1&&!result.purge_allowed,"new_account_invalid");account_password(request,result);}
    return result;
}
app::LocalAccountRequest account_action(std::string_view action,const Json& request){
    require(request.is_object()&&request.contains("revision"),"account_action_invalid");app::LocalAccountRequest result;
    result.account_id=account_record(request,false);result.expected_revision=account_revision(request.at("revision"));
    const bool reset=action=="reset_password";require(request.size()==(reset?4U:2U),"account_action_invalid");
    if(reset){result.action=app::LocalAccountAction::issue_temporary_password;account_password(request,result);}
    else if(action=="deactivate")result.action=app::LocalAccountAction::lock;
    else if(action=="reactivate")result.action=app::LocalAccountAction::unlock;
    else if(action=="delete")result.action=app::LocalAccountAction::tombstone;
    else throw std::runtime_error("account_action_invalid");
    return result;
}
Json system_password_page(const app::LocalAccountSnapshot& current){
    require(current.code==app::LocalAccountResultCode::ok&&current.system_revision>0,"system_password_unavailable");
    return {{"route","system.system_password"},{"title","System Password"},{"category","System"},{"revision",std::to_string(current.system_revision)},
        {"can_save",true},{"fields",Json::array()},{"active_fields",Json::array()},
        {"activation","Permanent System Password revision: "+std::to_string(current.system_revision)}};
}
app::LocalAccountRequest system_password_update(const Json& request){
    require(request.is_object()&&request.size()==3&&request.contains("revision"),"system_password_request_invalid");
    app::LocalAccountRequest result;result.action=app::LocalAccountAction::replace_system_password;
    result.expected_revision=account_revision(request.at("revision"));account_password(request,result);return result;
}
Json throttle_page(){
    Fields fields;unsigned scope=0;std::string subject;
    fields.integer("scope","Clear login delay for",scope,0,3);
    fields.items.back()["type"]="select";fields.items.back()["choices"]={{{"value",0},{"label","One IP address"}},{{"value",1},{"label","One user"}},{{"value",2},{"label","Shared System Password"}},{{"value",3},{"label","Shared BBS login delay"}}};
    fields.text("subject","IP address or user number",subject,45,"Use one canonical IP address or the user number shown in Accounts. Leave blank for a shared delay.");
    return {{"route","security.throttle_recovery"},{"title","Failed Login Attempts"},{"category","Security"},{"revision","1"},{"can_save",true},{"fields",fields.items},{"active_fields",fields.items},
        {"activation","Repeated wrong passwords cause temporary login delays, even with BotGate off. Delays expire automatically."}};
}
app::LocalThrottleClearRequest throttle_update(const Json& request){
    require(request.is_object()&&request.size()==2&&request.contains("scope")&&request.contains("subject"),"throttle_request_invalid");
    const auto& scope=request.at("scope");require(scope.is_number_integer()&&scope>=Json(0)&&scope<=Json(3)&&request.at("subject").is_string(),"throttle_request_invalid");
    const std::array<app::RateLimitScope,4> scopes{{app::RateLimitScope::source,app::RateLimitScope::account,app::RateLimitScope::system_password,app::RateLimitScope::global}};
    app::LocalThrottleClearRequest result{scopes[scope.get<unsigned>()],request.at("subject").get<std::string>()};
    require(app::valid_local_throttle_clear(result),"throttle_subject_invalid");return result;
}
