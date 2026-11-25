    api_get("/api/config/page/security.web_access",[&](const auto&,auto& response){
        std::lock_guard<std::mutex> lock(web_access_mutex);const auto settings=rt::web::read_web_access(root);Json fields=Json::array();
        const auto text_field=[&](const char* key,const char* label,const std::string& value,std::size_t maximum,const char* help,bool multiline=false){fields.push_back({{"key",key},{"label",label},{"type",multiline?"multiline":"text"},{"value",value},{"maximum",maximum},{"help",help}});};
        const auto flag=[&](const char* key,const char* label,bool value,const char* help){fields.push_back({{"key",key},{"label",label},{"type","checkbox"},{"value",value},{"help",help}});};
        flag("enabled","Allow approved computers on the LAN",settings.enabled,"Off on a fresh installation. LAN controls require HTTPS and the System Password. The local launcher remains available.");
        text_field("address","Listen on this Linux IP address",settings.address,45,"Use one numeric address assigned to this Linux computer. No wildcard or host name.");
        text_field("server_name","Browser server name (optional)",settings.server_name,253,"Leave blank to use the listening IP. For your own DNS certificate, enter its matching host name and make sure approved computers can resolve it.");
        fields.push_back({{"key","port"},{"label","HTTPS port"},{"type","number"},{"value",settings.port},{"minimum",1},{"maximum",65535},{"help","Use a free port. Ports below 1024 may require the optional service bind permission; SFX still runs as your normal account."}});
        std::string addresses;for(const auto& address:settings.approved){if(!addresses.empty())addresses+='\n';addresses+=address;}
        text_field("approved","Approved IP addresses",addresses,2048,"One exact IP per line, at most 32. Network ranges and proxy headers do not grant access. Removing this computer can end its session.",true);
        flag("custom_certificate","Use my own HTTPS certificate",settings.custom_certificate,"Otherwise SFX creates a private certificate. Verify its SHA-256 fingerprint before accepting it in your browser; no system trust settings are changed.");
        text_field("certificate_path","Import certificate file (optional)","",2048,"Absolute path on Linux to a private PEM certificate/chain file. Enter both paths to import; leave both blank to keep the installed identity.");
        text_field("key_path","Import private key file (optional)","",2048,"Absolute path on Linux to its unencrypted PEM key. Files and their folders must be private to your Linux account. SFX keeps its own private copy.");
        flag("regenerate","Replace the generated certificate on Save",false,"Turn LAN access off and save first. Replacement changes the fingerprint and requires fresh browser verification. Your custom certificate is kept.");
        flag("end_sessions","End all LAN browser sessions on Save",false,"Local launcher only. Frees remote session slots and disconnects their browser terminals. Other BBS callers and local browser access stay connected.");
        const std::string state=lan_active?"LAN HTTPS is active. ":"LAN HTTPS is not active. ";
        std::string selected_fingerprint;try{selected_fingerprint=rt::web::load_web_tls(root,settings).fingerprint;}catch(const std::exception&){}
        json_response(response,{{"route","security.web_access"},{"title","Web Access"},{"category","Security"},{"revision",std::to_string(settings.revision)},{"can_save",true},{"fields",fields},{"active_fields",Json::array()},
            {"activation",state+lan_detail+(selected_fingerprint.empty()?"":" Selected certificate SHA-256: "+selected_fingerprint)}});
    });
    api_post("/api/config/page/security.web_access",[&](const auto& request,auto& response){
        auto value=parse(request);require_keys(value,{"revision","values"});const auto& fields=value.at("values");
        require_keys(fields,{"enabled","address","server_name","port","approved","custom_certificate","certificate_path","key_path","regenerate","end_sessions"});
        for(const auto* flag:{"regenerate","end_sessions"})if(!fields.at(flag).is_boolean())throw std::runtime_error("web_access_switch_invalid");
        const auto expected=number(value.at("revision").template get<std::string>());if(!expected||expected>=UINT64_MAX-1)throw std::runtime_error("web_access_revision_invalid");
        auto ips=fields.at("approved").template get<std::string>();if(ips.size()>2048)throw std::runtime_error("web_access_acl_too_long");Json approved=Json::array();std::istringstream lines(ips);std::string line;
        while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(!line.empty())approved.push_back(line);}
        const auto next=rt::web::parse_web_access({{"revision",std::to_string(expected+1)},{"enabled",fields.at("enabled")},{"address",fields.at("address")},{"server_name",fields.at("server_name")},{"port",fields.at("port")},{"approved",approved},{"custom_certificate",fields.at("custom_certificate")}});
        const auto certificate=fields.at("certificate_path").template get<std::string>(),key=fields.at("key_path").template get<std::string>();
        if(certificate.size()>2048||key.size()>2048||certificate.empty()!=key.empty())throw std::runtime_error("web_certificate_import_paths_invalid");
        const bool regenerate=fields.at("regenerate").template get<bool>(),end_sessions=fields.at("end_sessions").template get<bool>();
        if(remote&&end_sessions){json_response(response,{{"detail","Use the local launcher to end all LAN sessions."}},403);return;}
        std::lock_guard<std::mutex> lock(web_access_mutex);const auto current=rt::web::read_web_access(root);
        if(current.revision!=expected){json_response(response,{{"detail","Web Access changed elsewhere. Reload before saving."}},409);return;}
        if(current.enabled&&(regenerate||!certificate.empty())){json_response(response,{{"detail","Turn LAN access off and save before importing or replacing a certificate. BBS callers can stay online."}},409);return;}
        try{
            const auto identity=next.server_name.empty()?next.address:next.server_name;
            if(!certificate.empty())rt::web::import_web_tls(root,identity,certificate,key);
            if(regenerate)rt::web::generate_web_tls(root,identity,true);
            if(next.enabled){
                if(runtime.account_snapshot({}).system_revision<=0)throw std::runtime_error("web_access_local_setup_required");
                if(!next.custom_certificate){const auto config=host::open_owned_directory(root/"config");struct stat info{};
                    if(fstatat(config.get(),"web-https-generated.v1",&info,AT_SYMLINK_NOFOLLOW)!=0){if(errno!=ENOENT)throw std::runtime_error("web_certificate_unreadable");if(current.enabled)throw std::runtime_error("web_missing_active_identity");rt::web::generate_web_tls(root,identity,false);}}
                (void)rt::web::load_web_tls(root,next);
            }
            rt::web::write_web_access(root,next,expected);if(end_sessions)remote_sessions.revoke_all();
            json_response(response,{{"ok",true},{"detail","Web Access saved. Refresh this page to check the actual listener and certificate fingerprint. BBS callers were not restarted."}});
        }catch(const std::exception& error){json_response(response,{{"ok",false},{"detail",std::string("Web Access could not be completed. Reload to check saved settings and certificate identity before another attempt. ")+rt::web::web_access_error(error.what())}},409);}
    });
