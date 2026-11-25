namespace {
constexpr const char* dos_profile_name="dos-runtime-linux.v1";
std::string dos_simple_path(const fs::path& path,bool empty=false){
    const auto value=path.string();need((empty&&value.empty())||(!value.empty()&&value.size()<=2048&&path.is_absolute()&&value.find_first_of(" \t\r\n\"\\=")==value.npos),"dos_setup_absolute_simple_path_required");
    for(const auto& part:path.relative_path())need(part!="."&&part!="..","dos_setup_path_alias_refused");
    return value;
}
std::string dos_bytes_hash(std::string_view value){
    need(sodium_init()>=0,"dos_setup_crypto_unavailable");std::array<unsigned char,32> hash{};
    crypto_hash_sha256(hash.data(),reinterpret_cast<const unsigned char*>(value.data()),value.size());std::array<char,65> hex{};sodium_bin2hex(hex.data(),hex.size(),hash.data(),hash.size());return hex.data();
}
bool dos_hash_valid(std::string_view s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string dos_lower(std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
std::string dos_trim(std::string s){const auto first=s.find_first_not_of(" \t\r");if(first==s.npos)return {};const auto last=s.find_last_not_of(" \t\r");return s.substr(first,last-first+1);}
std::string dos_profile_encode(const DosRuntimeProfile& p){
    need(p.revision&&p.revision<UINT64_MAX,"dos_setup_revision_invalid");
    need(dos_hash_valid(p.options.dosemu_sha256),"dos_setup_identity_invalid");
    return "Revision="+std::to_string(p.revision)+"\nRoot="+dos_simple_path(p.options.dosemu_root,true)+"\nBinary="+dos_simple_path(p.options.dosemu_binary)+"\nSha256="+p.options.dosemu_sha256+"\n";
}
}
DosRuntimeProfile read_dos_runtime_profile(const fs::path& root){
    struct stat exists{};if(lstat((root/"config"/dos_profile_name).c_str(),&exists)!=0){need(errno==ENOENT,"dos_setup_profile_unavailable");return {};}
    auto config=open_owned_directory(root/"config");const auto bytes=read_private_file(config.get(),dos_profile_name,8192);
    const std::string raw(bytes.begin(),bytes.end());std::map<std::string,std::string> values;std::istringstream stream(raw);std::string line;
    while(std::getline(stream,line)){const auto at=line.find('=');need(at!=line.npos&&values.emplace(line.substr(0,at),line.substr(at+1)).second,"dos_setup_profile_invalid");}
    need(values.size()==4&&values.count("Revision")&&values.count("Root")&&values.count("Binary")&&values.count("Sha256"),"dos_setup_profile_invalid");
    DosRuntimeProfile profile;const auto& number=values.at("Revision");const auto parsed=std::from_chars(number.data(),number.data()+number.size(),profile.revision);
    need(parsed.ec==std::errc{}&&parsed.ptr==number.data()+number.size()&&std::to_string(profile.revision)==number,"dos_setup_revision_invalid");
    profile.options.dosemu_root=values.at("Root");profile.options.dosemu_binary=values.at("Binary");profile.options.dosemu_sha256=values.at("Sha256");
    need(dos_profile_encode(profile)==raw,"dos_setup_profile_invalid");return profile;
}
void save_dos_runtime_profile(const fs::path& root,const DoorOptions& options,std::uint64_t expected){
    need(geteuid()!=0&&expected<UINT64_MAX-1,"dos_setup_owner_or_revision_invalid");InstallationLock lock;
    auto management=open_owned_directory(root/"management");(void)management;
    need(lock.acquire(root/"management","dos-runtime-linux.lock"),"dos_setup_settings_busy");
    need(read_dos_runtime_profile(root).revision==expected,"dos_setup_settings_changed");
    auto selected=dos_runtime(root,options);auto binary=selected.dosemu_binary;auto held=executable(binary,root);
    need(!options.dosemu_sha256.empty()&&digest(held.get())==options.dosemu_sha256,"dos_setup_runtime_changed_check_again");
    DosRuntimeProfile profile{expected+1,selected};const auto text=dos_profile_encode(profile);auto config=open_owned_directory(root/"config");
    publish_private_file(config.get(),dos_profile_name,{text.begin(),text.end()},true);
}
std::string run_dos_setup_command(const fs::path& path,std::vector<std::string> args,const fs::path& working,
    const std::function<bool()>& cancel,const std::function<void(std::string_view)>& output,unsigned seconds,const DoorOptions& options){
    need(geteuid()!=0&&seconds>=1&&seconds<=900,"dos_setup_command_context_invalid");
    if(cancel&&cancel())throw std::runtime_error("dos_setup_cancelled");
    auto executable_path=path;auto program=executable(executable_path,working);auto cwd=directory(working);
    if(!options.dosemu_sha256.empty())need(digest(program.get())==options.dosemu_sha256,"dos_setup_runtime_changed_check_again");
    args.insert(args.begin(),executable_path.string());std::vector<std::string> env{"PATH=/usr/bin:/bin","LANG=C","HOME="+working.string(),"PYTHONUNBUFFERED=1"};
    if(!options.dosemu_root.empty()){
        const auto& base=options.dosemu_root;env.push_back("LD_LIBRARY_PATH="+(base/"usr/lib/x86_64-linux-gnu").string()+":"+(base/"usr/lib/dosemu").string());
        env.push_back("DOSEMU2_COMCOM_DIR="+(base/"usr/share/comcom32").string());env.push_back("FDPP_KERNEL_DIR="+(base/"usr/share/fdpp").string());
    }
    PipeSignals signals;auto input=pipe_pair(true),out=pipe_pair(false),error=pipe_pair(false);Child child;
    spawn(child,program,cwd,args,env,input.child.get(),out.child.get(),error.child.get(),-1);input.child.reset();input.parent.reset();out.child.reset();error.child.reset();
    std::string result;const auto deadline=Clock::now()+std::chrono::seconds(seconds);bool exited{};
    for(;;){
        need(!(cancel&&cancel()),"dos_setup_cancelled");need(Clock::now()<deadline,"dos_setup_command_timed_out");
        pollfd ready[2]{{out.parent.get(),POLLIN,0},{error.parent.get(),POLLIN,0}};need(poll(ready,2,20)>=0||errno==EINTR,"dos_setup_output_failed");
        bool read_any{};for(const auto& p:ready)if(p.revents&(POLLIN|POLLHUP)){
            std::array<char,4096> buffer{};const auto n=read(p.fd,buffer.data(),buffer.size());if(n>0){read_any=true;need(result.size()+static_cast<std::size_t>(n)<=32768,"dos_setup_output_too_large");const auto part=std::string_view(buffer.data(),static_cast<std::size_t>(n));result+=part;if(output)output(part);}
            else need(n==0||errno==EAGAIN||errno==EINTR,"dos_setup_output_failed");
        }
        if(!exited)exited=child.exited();
        if(exited&&!read_any)break;
    }
    need(child.stop(),"dos_setup_child_cleanup_failed");
    if(!WIFEXITED(child.status)||WEXITSTATUS(child.status)!=0)throw std::runtime_error("dos_setup_command_failed: "+result.substr(result.size()>1536?result.size()-1536:0));
    return result;
}
DosRuntimeCheck check_dos_runtime(const fs::path& root,DoorOptions options,const std::function<bool()>& cancel){
    need(geteuid()!=0,"dos_setup_normal_account_required");options=dos_runtime(root,std::move(options));dos_simple_path(options.dosemu_root,true);dos_simple_path(options.dosemu_binary);
    auto path=options.dosemu_binary;auto program=executable(path,root);std::array<unsigned char,20> elf{};
    need(pread(program.get(),elf.data(),elf.size(),0)==static_cast<ssize_t>(elf.size())&&elf[4]==2&&elf[5]==1&&elf[18]==62&&elf[19]==0,"dos_setup_amd64_elf_required");
    struct stat identity{};need(!fstat(program.get(),&identity)&&(identity.st_uid==0||identity.st_uid==geteuid()),"dos_setup_runtime_owner_invalid");
    const auto sha=digest(program.get());if(!options.dosemu_sha256.empty())need(sha==options.dosemu_sha256,"dos_setup_runtime_changed_check_again");
    if(options.dosemu_root.empty())for(const auto* resource:{"/usr/share/dosemu","/usr/share/comcom32","/usr/share/fdpp"}){auto pin=directory(resource);(void)pin;}
    options.dosemu_sha256=sha;
    const auto version=run_dos_setup_command(path,{"--version"},root,cancel,{},5,options);const auto lower=dos_lower(version);
    need(lower.find("dosemu2")!=lower.npos||lower.find("dosemu 2.")!=lower.npos,"dos_setup_dosemu2_version_not_recognized");
    // Re-pin after the probe: the saved choice must be the bytes that were checked.
    auto after=executable(path,root);need(digest(after.get())==sha,"dos_setup_runtime_changed_check_again");
    options.dosemu_sha256=sha;return {options,version.substr(0,1024)};
}
DosGameCheck inspect_dos_game(const fs::path& root,const fs::path& requested,std::optional<unsigned> input_interval_ms){
    need(!input_interval_ms||*input_interval_ms<=100,"dos_setup_input_interval_out_of_range");
    const auto path=requested.is_absolute()?requested:root/requested;dos_simple_path(path);auto dir=open_owned_directory(path,false);
    DosGameCheck result;result.directory=path;std::size_t count{};
    for(const auto& item:fs::directory_iterator(path)){
        need(++count<=4096,"dos_setup_game_folder_too_large");const auto extension=dos_lower(item.path().extension().string());
        if(extension!=".exe"&&extension!=".com"&&extension!=".bat")continue;
        struct stat s{};const auto name=item.path().filename().string();if(fstatat(dir.get(),name.c_str(),&s,AT_SYMLINK_NOFOLLOW)!=0||!S_ISREG(s.st_mode))continue;
        if(name.size()<=255&&name.find_first_of("\r\n\0") == name.npos){need(result.programs.size()<256,"dos_setup_too_many_programs");result.programs.push_back(name);}
    }
    std::sort(result.programs.begin(),result.programs.end());struct stat saved{};const bool exists=fstatat(dir.get(),"sfxexec.ini",&saved,AT_SYMLINK_NOFOLLOW)==0;
    if(!exists)need(errno==ENOENT,"dos_setup_game_settings_unavailable");else need(S_ISREG(saved.st_mode)&&saved.st_uid==geteuid()&&!(saved.st_mode&0022),"dos_setup_game_settings_permissions");
    const auto startup=file_text(path/"sfxexec.ini");result.driver_identity=exists?dos_bytes_hash(startup):"absent";
    std::string diagnostic;const auto settings=app::parse_door_driver_settings(file_text(root/"bin"/"sfxexec.ini"),startup,&diagnostic);need(bool(settings),diagnostic.c_str());
    const auto own_settings=app::parse_door_driver_settings("",startup,&diagnostic);need(bool(own_settings),diagnostic.c_str());
    result.input_interval_ms=input_interval_ms.value_or(settings->dos_input_interval_ms.value_or(0));
    const bool change_interval=input_interval_ms&&(!own_settings->dos_input_interval_ms||*input_interval_ms!=*own_settings->dos_input_interval_ms);
    result.driver_text=startup;result.driver_change=settings->can_disconnect||change_interval;if(!result.driver_change)return result;
    std::istringstream lines(startup);std::string line,updated;bool replaced{},interval_replaced{};
    while(std::getline(lines,line)){
        const auto at=line.find('=');if(at!=line.npos&&dos_lower(dos_trim(line.substr(0,at)))=="candisconnect"){line="CanDisconnect=false";replaced=true;}
        if(change_interval&&at!=line.npos&&dos_lower(dos_trim(line.substr(0,at)))=="dosinputintervalms"){line="DosInputIntervalMs="+std::to_string(*input_interval_ms);interval_replaced=true;}
        updated+=line+'\n';
    }
    if(!replaced||(change_interval&&!interval_replaced)){
        updated+="\n[SFXEXEC]\n";
        if(!replaced)updated+="CanDisconnect=false\n";
        if(change_interval&&!interval_replaced)updated+="DosInputIntervalMs="+std::to_string(*input_interval_ms)+'\n';
    }
    const auto checked=app::parse_door_driver_settings(file_text(root/"bin"/"sfxexec.ini"),updated,&diagnostic);need(checked&&!checked->can_disconnect&&checked->dos_input_interval_ms.value_or(0)==result.input_interval_ms,"dos_setup_driver_setting_invalid");result.driver_text=updated;return result;
}
void prepare_dos_game_driver(const fs::path& root,const fs::path& startup,std::string_view expected,std::optional<unsigned> input_interval_ms){
    auto lock_directory=open_owned_directory(startup,false);
    FileDescriptor lock(openat(lock_directory.get(),".sfx-dosemu-driver.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600));
    need(private_regular_file(lock.get(),4096)&&flock(lock.get(),LOCK_EX|LOCK_NB)==0,"dos_setup_game_settings_busy");
    const auto checked=inspect_dos_game(root,startup,input_interval_ms);
    need(checked.driver_identity==expected,"dos_setup_game_settings_changed_check_again");if(!checked.driver_change)return;
    auto dir=open_owned_directory(checked.directory,false);publish_private_file(dir.get(),"sfxexec.ini",{checked.driver_text.begin(),checked.driver_text.end()},expected!="absent");
}
