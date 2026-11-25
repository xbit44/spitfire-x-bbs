namespace {
std::string digest_hex(const app::FtnDigest& digest){std::string result;for(auto b:digest){result+="0123456789abcdef"[b>>4];result+="0123456789abcdef"[b&15];}return result;}
app::FtnDigest digest_text(std::string_view text){app::FtnDigest result{};crypto_hash_sha256(result.data(),reinterpret_cast<const unsigned char*>(text.data()),text.size());return result;}
bool run_name(std::string_view run){return run.size()==32&&std::all_of(run.begin(),run.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})&&run.find_first_not_of('0')!=run.npos;}
struct ProfileErase {std::string& text;~ProfileErase(){if(!text.empty())sodium_memzero(text.data(),text.size());}};
void private_profile_root(const std::filesystem::path& root){
#ifdef _WIN32
    require(win32::WindowsStorageDaclPolicy(win32::current_process_user_sid_string()).assess(root/"FTN",win32::StorageDaclObjectKind::directory).status.succeeded(),"ftn_external_ftn_folder_not_private");
#else
    struct stat state{};require(lstat((root/"ftn").c_str(),&state)==0&&S_ISDIR(state.st_mode)&&state.st_uid==geteuid()&&(state.st_mode&0077)==0,"ftn_external_ftn_folder_not_private");
#endif
}
std::string read_secret_profile(const std::filesystem::path& root,std::string_view relative){
    private_profile_root(root);File file(checked(root,relative),false,false,true);require(file.size()<=65536,"ftn_external_profile_too_large");
    std::string result;std::array<char,4096> buffer{};
    struct Erase {decltype(buffer)& bytes;~Erase(){sodium_memzero(bytes.data(),bytes.size());}} erase{buffer};
    for(;;){const auto count=file.read(buffer.data(),buffer.size());if(!count)break;require(count<=65536-result.size(),"ftn_external_profile_too_large");result.append(buffer.data(),count);}file.verify();return result;
}
struct RunManifest {std::string profile_hash;std::vector<app::FtnAddress> protected_remote;app::FtnDigest digest;};
std::string lower_leaf(std::string value){for(auto& c:value)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);return value;}
RunManifest read_run_manifest(const std::filesystem::path& root,std::string_view run){
    private_profile_root(root);
    require(run_name(run),"ftn_external_run_invalid");const auto text=read_small(root,"FTN/External/Runs/"+std::string(run)+"/Manifest",65536);
    std::vector<std::string_view> lines;std::string_view rest=text;
    while(!rest.empty()){const auto end=rest.find('\n');require(end!=rest.npos&&lines.size()<1024,"ftn_external_run_manifest_invalid");lines.push_back(rest.substr(0,end));rest.remove_prefix(end+1);}
    require(lines.size()>=3&&lines[0]=="SFX-BINKD-RUN-1"&&lines[1]==run&&lines[2].size()==64,"ftn_external_run_manifest_invalid");
    RunManifest result;result.profile_hash=lines[2];result.digest=digest_text(text);
    for(std::size_t i=3;i<lines.size();++i){require(lines[i].substr(0,2)=="A ","ftn_external_run_manifest_invalid");const auto address=app::parse_ftn_address(lines[i].substr(2));require(address.has_value()&&!address->domain.empty(),"ftn_external_run_manifest_invalid");result.protected_remote.push_back(*address);}
    return result;
}
void verify_run_record(const std::filesystem::path& root,const app::FtnExternalReceiveRecord& record){
    require(!record.run.empty(),"ftn_external_unbound_session_retained");const auto manifest=read_run_manifest(root,record.run);
    require(manifest.digest==record.run_digest,"ftn_external_run_record_conflict");
    std::vector<app::FtnAddress> expected;
    if(record.protected_session)for(const auto& claimed:record.remote)if(std::any_of(manifest.protected_remote.begin(),manifest.protected_remote.end(),[&](const auto& a){return app::same_ftn_address(a,claimed,false);}))expected.push_back(claimed);
    require(expected.size()==record.authenticated_remote.size(),"ftn_external_alias_proof_conflict");
    for(std::size_t i=0;i<expected.size();++i)require(app::same_ftn_address(expected[i],record.authenticated_remote[i],false),"ftn_external_alias_proof_conflict");
}
}
RunProfile prepare_run(const std::filesystem::path& root,std::string_view source_profile,std::string_view recorder){
#ifdef _WIN32
    // Sysop preparation creates the secret snapshot. An independently managed
    // System mailer may use an already prepared run, but is not its owner.
    require(win32::current_process_user_sid_string()!=L"S-1-5-18","ftn_external_prepare_requires_sysop_account");
#endif
    require(app::valid_ftn_external_path(recorder)&&recorder.find('/')!=0,"ftn_external_recorder_path_invalid");
    auto text=read_secret_profile(root,canonical_relative(source_profile));ProfileErase erase{text};auto profile=app::parse_ftn_external_profile(text,path_style);require(profile.has_value(),"ftn_external_profile_unsupported");ProfileErase erase_config{profile->configuration};
    for(const auto& network:profile->networks)for(const auto* leaf:{"Copies","Outbound","Journal"})(void)checked(root,app::ftn_external_root(network.code)+"/"+leaf+"/probe",true);
    for(const auto* leaf:{"Inbound/Secure","Inbound/Nonsecure","Temp","Records","Consumed","Runs"})(void)checked(root,std::string("FTN/External/")+leaf+"/probe",true);
    app::Identifier128 identifier;randombytes_buf(identifier.bytes.data(),identifier.bytes.size());require(identifier.valid(),"ftn_external_run_random_failed");const auto run=token(identifier);
    const auto base="FTN/External/Runs/"+run;auto command=std::string(recorder);
#ifdef _WIN32
    std::string escaped;for(char c:command){if(c=='/')escaped+="\\\\";else escaped+=c;}command=std::move(escaped);
#endif
    profile->configuration+="log \""+native_relative(base+"/binkd.log")+"\"\nloglevel 4\nconlog 0\nexec \"!"+command+" --record-run "+run+" *F *P *A*\" \""+native_relative("FTN/External/Inbound/probe").substr(0,native_relative("FTN/External/Inbound/probe").size()-5)+"*\"\n";
    const auto config=base+"/binkd.cfg";publish_once(root,config,profile->configuration);
    std::string manifest="SFX-BINKD-RUN-1\n"+run+"\n"+digest_hex(digest_text(profile->configuration))+"\n";
    for(const auto& address:profile->protected_remote)manifest+="A "+app::format_ftn_address(address)+"\n";
    publish_once(root,base+"/Manifest",manifest);return {run,native_relative(config)};
}
app::FtnExternalReceiveRecord record_run_completed(const std::filesystem::path& root,std::string_view run,std::string relative,bool protected_session,const std::vector<app::FtnAddress>& remote,const std::function<void(std::string_view)>& boundary){
    const auto manifest=read_run_manifest(root,run);auto profile=read_secret_profile(root,"FTN/External/Runs/"+std::string(run)+"/binkd.cfg");ProfileErase erase{profile};
    require(digest_hex(digest_text(profile))==manifest.profile_hash,"ftn_external_active_profile_changed");
    app::FtnExternalReceiveRecord record;record.id.bytes[0]=1;record.run=run;record.run_digest=manifest.digest;record.protected_session=protected_session;record.remote=remote;
    std::replace(relative.begin(),relative.end(),'\\','/');record.relative_file=canonical_relative(relative);
    if(protected_session)for(const auto& claimed:remote)if(std::any_of(manifest.protected_remote.begin(),manifest.protected_remote.end(),[&](const auto& a){return app::same_ftn_address(a,claimed,false);}))record.authenticated_remote.push_back(claimed);
    require(app::encode_ftn_external_receive(record).has_value(),"ftn_external_hook_context_invalid");
    File file(checked(root,record.relative_file));record.size=file.size();record.timestamp=file.timestamp();record.digest=hash_file(file);
    const auto initial=app::encode_ftn_external_receive(record);require(initial.has_value(),"ftn_external_hook_context_invalid");const auto identity=digest_text(*initial);std::copy_n(identity.begin(),16,record.id.bytes.begin());
    const auto encoded=app::encode_ftn_external_receive(record);require(encoded.has_value(),"ftn_external_record_invalid");
    if(lower_leaf(std::filesystem::u8path(record.relative_file).extension().u8string())==".tic"&&record.size<=65536){
        std::string bytes;ProfileErase erase_tic{bytes};File tic_file(checked(root,record.relative_file));
        require(hash_file(tic_file,[&](auto part){bytes.append(part);})==record.digest,"ftn_external_tic_changed");
        auto tic=app::decode_ftn_tic(bytes);
        if(tic.tic){ProfileErase erase_password{tic.tic->password};const auto name=lower_leaf(tic.tic->file);
            require(app::valid_ftn_external_path(name)&&name.find('/')==name.npos,"ftn_external_tic_payload_name_unsupported");
            // Publish index first; readers require the matching Records entry,
            // so an interrupted hook cannot grant authority through an orphan.
            publish_once(root,"FTN/External/Tics/"+name+"/"+token(record.id)+".rec",*encoded);
            if(boundary)boundary("after-tic-index-before-record");
        }
    }
    publish_once(root,record_path(record,"Records"),*encoded);return record;
}
std::vector<app::FtnExternalReceiveRecord> tic_records_for_payload(const std::filesystem::path& root,std::string_view filename,const std::function<void(std::string_view,std::string_view)>& report){
    const auto name=lower_leaf(std::string(filename));require(app::valid_ftn_external_path(name)&&name.find('/')==name.npos,"ftn_external_payload_name_unsupported");
    const auto base="FTN/External/Tics/"+name;
    // Creating an empty bounded index directory is harmless; it avoids treating
    // an ordinary payload-before-TIC arrival as a filesystem failure.
    const auto folder=checked(root,base+"/probe",true).parent_path();std::vector<app::FtnExternalReceiveRecord> result;std::size_t examined{};
    for(const auto& entry:std::filesystem::directory_iterator(folder))if(entry.path().extension()==".rec"){
        require(++examined<=128,"ftn_external_tic_route_limit");const auto leaf=entry.path().filename().u8string();const auto label=leaf.size()==36&&run_name(std::string_view(leaf).substr(0,32))?leaf:std::string("invalid-record");
        try{
            const auto encoded=read_small(root,base+"/"+leaf);
            const auto record=app::decode_ftn_external_receive(encoded);require(record&&!record->run.empty()&&token(record->id)+".rec"==leaf,"ftn_external_tic_index_invalid");
            verify_run_record(root,*record);require(read_small(root,record_path(*record,"Records"))==encoded,"ftn_external_tic_index_orphan");result.push_back(*record);if(report)report(label,{});
        }catch(const std::exception&){if(report)report(label,"ftn_external_tic_index_retained_for_review");}
    }
    return result;
}
void audit_inbox(const std::filesystem::path& root,const std::function<void(std::string_view,std::string_view)>& report){
    require(static_cast<bool>(report),"ftn_external_audit_report_missing");
    // Bounds are diagnostic-only. Intake retains its independent paged cursor.
    // No directory scan synthesizes authentication or a receipt.
    std::map<std::string,std::vector<std::pair<std::uint64_t,std::uint64_t>>> recorded;
    const auto records=checked(root,"FTN/External/Records/probe").parent_path();std::size_t count{};
    for(const auto& entry:std::filesystem::directory_iterator(records)){
        if(++count>4096){report("External inbox audit","ftn_external_audit_record_limit");return;}
        if(entry.path().extension()!=".rec")continue;
        try{
            const auto name=entry.path().filename().u8string();const auto record=app::decode_ftn_external_receive(read_small(root,"FTN/External/Records/"+name));
            require(record&&!record->run.empty()&&token(record->id)+".rec"==name,"ftn_external_record_invalid");verify_run_record(root,*record);
            recorded[record->relative_file].push_back({record->size,record->timestamp});
        }catch(const std::exception&){report("External receive records","ftn_external_record_retained_for_review");}
    }
    for(const auto* leaf:{"Secure","Nonsecure"}){
        const auto base=std::string("FTN/External/Inbound/")+leaf;const auto folder=checked(root,base+"/probe").parent_path();count=0;
        for(const auto& entry:std::filesystem::directory_iterator(folder)){
            if(++count>4096){report(base,"ftn_external_audit_file_limit");break;}
            const auto relative=base+"/"+entry.path().filename().u8string();std::string label=relative;
            for(auto& ch:label)if(static_cast<unsigned char>(ch)<32||static_cast<unsigned char>(ch)>126)ch='?';
            if(label.size()>180)label.resize(180);
            if(!app::valid_ftn_external_path(relative)){report(label,"ftn_external_hook_filename_unsupported");continue;}
            try{
                File file(checked(root,relative));const auto found=recorded.find(relative);const auto identity=std::make_pair(file.size(),file.timestamp());file.verify();
                const bool present=found!=recorded.end()&&std::find(found->second.begin(),found->second.end(),identity)!=found->second.end();
                report(label,present?std::string_view{}:std::string_view{"ftn_external_hook_metadata_missing"});
            }catch(const std::exception&){report(label,"ftn_external_inbox_file_retained_for_review");}
        }
    }
}
