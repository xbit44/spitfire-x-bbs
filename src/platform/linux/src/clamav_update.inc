namespace sfx::platform::linux_host {
namespace {
using UpdateMode=LinuxClamAvUpdateMode;
using DefinitionChange=LinuxClamAvDefinitionChange;
constexpr std::uint64_t update_bound=2ULL*1024*1024*1024;
const std::array<std::string,3> database_bases{{"main","daily","bytecode"}};
std::string descriptor_path(int fd){return "/proc/self/fd/"+std::to_string(fd);}
std::int64_t decimal(const std::string& text,std::int64_t maximum=32503680000LL){
    std::int64_t value{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&value>=0&&value<=maximum&&std::to_string(value)==text,"clamav_status_invalid");return value;
}
struct UpdateHistory {
    std::int64_t attempt{},success{},changed{};
    UpdateMode mode{UpdateMode::automatic};DefinitionChange change{DefinitionChange::unknown};
    std::string result{"clamav_update_unknown"};std::array<std::string,3> fingerprints{{"-","-","-"}};
};
std::string encode(const UpdateHistory& h){
    std::string text="SFX-LINUX-CLAMAV-STATUS-1\n"+std::to_string(h.attempt)+"\n"+std::to_string(h.success)+"\n"+std::to_string(h.changed)+"\n"+
        (h.mode==UpdateMode::manual?"manual":"automatic")+"\n"+std::to_string(static_cast<unsigned>(h.change))+"\n"+h.result+"\n";
    for(const auto& f:h.fingerprints)text+=f+"\n";
    return text;
}
bool absent(int dir,const char* name){struct stat info{};if(fstatat(dir,name,&info,AT_SYMLINK_NOFOLLOW)==0)return false;require(errno==ENOENT,"clamav_object_stat");return true;}
std::optional<UpdateHistory> history(int dir){
    if(absent(dir,"update-status.v1"))return {};
    const auto bytes=read_private_file(dir,"update-status.v1",1024);const std::string text(bytes.begin(),bytes.end());
    require(!text.empty()&&text.back()=='\n',"clamav_status_invalid");std::istringstream stream(text);std::vector<std::string> lines;std::string line;
    while(std::getline(stream,line))lines.push_back(line);
    require(lines.size()==10&&lines[0]=="SFX-LINUX-CLAMAV-STATUS-1","clamav_status_invalid");UpdateHistory h;
    h.attempt=decimal(lines[1]);h.success=decimal(lines[2]);h.changed=decimal(lines[3]);
    require(h.attempt>0&&h.success<=h.attempt&&h.changed<=h.success&&(lines[4]=="manual"||lines[4]=="automatic"),"clamav_status_invalid");
    h.mode=lines[4]=="manual"?UpdateMode::manual:UpdateMode::automatic;h.change=static_cast<DefinitionChange>(decimal(lines[5],3));h.result=lines[6];
    require(!h.result.empty()&&h.result.size()<100&&h.result.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos,"clamav_status_invalid");
    for(std::size_t i=0;i<3;++i){h.fingerprints[i]=lines[7+i];const auto& f=h.fingerprints[i];if(!h.success){require(f=="-","clamav_status_invalid");continue;}
        const auto a=f.find('|'),b=f.find('|',a==std::string::npos?0:a+1);
        require(a!=std::string::npos&&b!=std::string::npos&&(f.substr(0,a)==database_bases[i]+".cvd"||f.substr(0,a)==database_bases[i]+".cld")&&decimal(f.substr(a+1,b-a-1),512LL*1024*1024)>0&&f.size()-b-1==64&&f.substr(b+1).find_first_not_of("0123456789abcdef")==std::string::npos,"clamav_status_invalid");}
    require((h.success>0||(h.changed==0&&h.change==DefinitionChange::unknown))&&encode(h)==text,"clamav_status_invalid");return h;
}
void save_history(int directory,const UpdateHistory& h){const auto text=encode(h);publish_private_file(directory,"update-status.v1",{text.begin(),text.end()},true);}
std::vector<Pinned> pin_engine(int directory,const std::function<bool()>& live){
    for(const auto& name:names(directory))require(name=="Licenses"||std::any_of(std::begin(programs),std::end(programs),[&](const auto& p){return name==p.name;}),"clamav_unpinned_engine_entry");
    std::vector<Pinned> result;for(const auto& program:programs){result.emplace_back(directory,program.name,program.bytes,live);
        require(static_cast<std::uint64_t>(result.back().original.st_size)==program.bytes&&result.back().digest==program.sha256,"clamav_engine_hash");}
    for(std::size_t i=0;i<3;++i)require((result[i].original.st_mode&S_IXUSR)!=0,"clamav_engine_not_executable");
    return result;
}
bool database_name(const std::string& name){if(name=="freshclam.dat")return true;for(const auto& base:database_bases)if(name==base+".cvd"||name==base+".cld")return true;return false;}
std::vector<Pinned> pin_databases(int directory,const std::function<bool()>& live,bool complete){
    const auto listed=names(directory);std::vector<Pinned> result;
    for(const auto& name:listed)require(database_name(name),"clamav_unexpected_database");
    for(const auto& base:database_bases){std::string selected;for(const auto* ext:{".cvd",".cld"})if(std::find(listed.begin(),listed.end(),base+ext)!=listed.end()){require(selected.empty(),"clamav_duplicate_database");selected=base+ext;}
        require(!complete||!selected.empty(),"clamav_database_missing");if(!selected.empty()){result.emplace_back(directory,selected,512ULL*1024*1024,live);require(result.back().original.st_size>0,"clamav_database_empty");}}
    if(std::find(listed.begin(),listed.end(),"freshclam.dat")!=listed.end())result.emplace_back(directory,"freshclam.dat",4096,live);
    return result;
}
std::array<std::string,3> fingerprints(const std::vector<Pinned>& pins){
    std::array<std::string,3> result;for(std::size_t i=0;i<3;++i){require(pins.size()>i&&pins[i].name.rfind(database_bases[i]+".",0)==0,"clamav_database_missing");
        result[i]=pins[i].name+"|"+std::to_string(pins[i].original.st_size)+"|"+pins[i].digest;}return result;
}
std::optional<LinuxClamAvDatabaseVersion> database_version(const Pinned& pin){
    try{std::array<char,512> bytes{};if(pread(pin.file.get(),bytes.data(),bytes.size(),0)!=512)return {};std::string head(bytes.data(),bytes.size());
        while(!head.empty()&&(head.back()==' '||head.back()=='\r'||head.back()=='\n'))head.pop_back();
        if(head.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789: +-_/=.")!=std::string::npos)return {};
        std::istringstream stream(head);std::vector<std::string> fields;std::string field;while(std::getline(stream,field,':'))fields.push_back(field);
        if(fields.size()!=9||fields[0]!="ClamAV-VDB")return {};
        const auto version=decimal(fields[2],UINT32_MAX),signatures=decimal(fields[3],UINT32_MAX),published=decimal(fields[8]);if(!version||!published)return {};
        return LinuxClamAvDatabaseVersion{static_cast<std::uint32_t>(version),static_cast<std::uint32_t>(signatures),published};
    }catch(...){return {};}
}
FD operation_gate(int directory){FD gate(openat(directory,"operation.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600));require(private_regular_file(gate.get(),0),"clamav_gate_private");return gate;}
void copy_pin(const Pinned& source,int destination,const std::function<void()>& poll){
    FD file(openat(destination,source.name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));require(bool(file),"clamav_copy_create");
    std::array<char,65536> bytes{};off_t offset{};while(offset<source.original.st_size){poll();const auto n=pread(source.file.get(),bytes.data(),bytes.size(),offset);
        if(n<0&&errno==EINTR)continue;
        require(n>0,"clamav_copy_read");ssize_t written{};while(written<n){const auto count=write(file.get(),bytes.data()+written,static_cast<std::size_t>(n-written));if(count<0&&errno==EINTR)continue;require(count>0,"clamav_copy_write");written+=count;}offset+=n;}
    source.verify();require(fsync(file.get())==0,"clamav_copy_flush");
}
void bounded_update_tree(int directory,unsigned depth,std::uint64_t& bytes,std::size_t& entries){
    require(depth<=12,"clamav_update_tree_depth");for(const auto& name:names(directory)){
        require(++entries<=8192,"clamav_update_tree_entries");FD file(openat(directory,name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));struct stat info{};
        require(bool(file)&&fstat(file.get(),&info)==0&&info.st_uid==geteuid()&&(info.st_mode&077)==0,"clamav_update_tree_private");
        if(S_ISDIR(info.st_mode))bounded_update_tree(file.get(),depth+1,bytes,entries);
        else {require(S_ISREG(info.st_mode)&&info.st_nlink==1&&info.st_size>=0&&static_cast<std::uint64_t>(info.st_size)<=update_bound-bytes,"clamav_update_tree_bound");bytes+=static_cast<std::uint64_t>(info.st_size);}}
}
struct UpdatePipe {
    Pipe pipe;std::string tail;std::uint64_t total{};
    void drain(){std::array<char,4096> block{};std::size_t read_count{};while(!pipe.eof&&read_count<65536){const auto n=read(pipe.read_end.get(),block.data(),block.size());
        if(n>0){read_count+=static_cast<std::size_t>(n);total+=static_cast<std::uint64_t>(n);for(ssize_t i=0;i<n;++i){const unsigned char c=static_cast<unsigned char>(block[static_cast<std::size_t>(i)]);tail+=c=='\n'||c=='\r'||c=='\t'||(c>=32&&c<127)?static_cast<char>(c):'?';}if(tail.size()>32768)tail.erase(0,tail.size()-32768);}
        else if(!n)pipe.eof=true;else if(errno==EINTR)continue;else if(errno==EAGAIN||errno==EWOULDBLOCK)break;else throw std::runtime_error("clamav_update_pipe");}}
};
std::uint32_t run_vendor(std::size_t program, int engine,std::vector<Pinned>& images,Scratch& scratch,int gate,
    std::vector<int> allowed,std::vector<std::string> arguments,const std::function<void()>& poll,
    const std::function<void()>& pending,bool& reaped,std::string& transcript){
    std::vector<char*> argv;for(auto& arg:arguments)argv.push_back(arg.data());argv.push_back(nullptr);
    std::string libraries="LD_LIBRARY_PATH="+descriptor_path(engine),temporary="TMPDIR="+descriptor_path(scratch.directory.get());char locale[]="LC_ALL=C";char* environment[]{locale,libraries.data(),temporary.data(),nullptr};
    UpdatePipe output,error;FD null(open("/dev/null",O_RDONLY|O_CLOEXEC));require(bool(null),"clamav_update_input");
    const int executable=images[program].file.get();allowed.insert(allowed.end(),{engine,scratch.directory.get(),scratch.budget.get(),gate,executable});std::sort(allowed.begin(),allowed.end());allowed.erase(std::unique(allowed.begin(),allowed.end()),allowed.end());
    rlimit limit{};require(getrlimit(RLIMIT_NOFILE,&limit)==0&&limit.rlim_max<=1048576&&allowed.back()<static_cast<int>(limit.rlim_max),"clamav_update_descriptors");
    for(const auto& image:images)image.verify();
    poll();const auto parent=getpid();Child child;child.observed=&reaped;child.pid=fork();require(child.pid>=0,"clamav_update_fork");
    if(child.pid==0){
        const rlimit children{0,0},core{0,0},memory{3ULL*1024*1024*1024,3ULL*1024*1024*1024},files{update_bound,update_bound};umask(0077);
        if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent||dup2(null.get(),0)<0||dup2(output.pipe.write_end.get(),1)<0||dup2(error.pipe.write_end.get(),2)<0||fchdir(scratch.directory.get())||!drop_child_capabilities()||setrlimit(RLIMIT_CORE,&core)||setrlimit(RLIMIT_AS,&memory)||setrlimit(RLIMIT_FSIZE,&files))_exit(126);
        // FreshClam's HTTPS resolver creates a thread. RLIMIT_NPROC=0 also
        // forbids threads and produced a real 15-second connection timeout.
        // Allow same-process threads while preserving the no-child-process rule.
        if(program==1?!restrict_child_to_threads():setrlimit(RLIMIT_NPROC,&children)!=0)_exit(126);
        for(const auto fd:allowed)if(fd!=executable&&fcntl(fd,F_SETFD,0)<0)_exit(126);
        if(!close_except(allowed,static_cast<unsigned>(limit.rlim_max)))_exit(126);
        fexecve(executable,argv.data(),environment);_exit(127);
    }
    reaped=false;output.pipe.write_end=FD();error.pipe.write_end=FD();std::uint64_t checked{};
    try{for(;;){output.drain();error.drain();poll();if(child.poll()){output.drain();error.drain();require(output.pipe.eof&&error.pipe.eof,"clamav_update_output_incomplete");break;}
        if(now()-checked>=250){std::uint64_t bytes{};std::size_t entries{};bounded_update_tree(scratch.directory.get(),0,bytes,entries);pending();checked=now();}
        pollfd ready[]{{output.pipe.read_end.get(),POLLIN,0},{error.pipe.read_end.get(),POLLIN,0}};(void)::poll(ready,2,25);}
    }catch(...){child.stop();output.drain();error.drain();transcript+=output.tail+error.tail;throw;}
    transcript+=output.tail+error.tail;require(program!=0||(output.total<=32768&&error.total<=32768),"clamav_update_verification_output_limit");poll();for(const auto& image:images)image.verify();
    return WIFEXITED(child.status)?static_cast<std::uint32_t>(WEXITSTATUS(child.status)):0xffffffffU;
}
bool erase_flat(int directory)noexcept{
    try{std::vector<Pinned> files;for(const auto& name:names(directory))files.emplace_back(directory,name,update_bound,[]{return true;});
        for(const auto& file:files){file.verify();require(unlinkat(directory,file.name.c_str(),0)==0,"clamav_cleanup_unlink");}return fsync(directory)==0;
    }catch(...){return false;}
}
}
void initialize_linux_clamav_data(const std::filesystem::path& root){
    require(geteuid()!=0,"clamav_normal_owner_required");auto engine=open_owned_directory(root/"bin/clamav");const auto pins=pin_engine(engine.get(),[]{return true;});
    for(const auto* parent:{"data","work"}){auto directory=open_owned_directory(root/parent);if(mkdirat(directory.get(),"clamav",0700)!=0)require(errno==EEXIST,"clamav_directory_create");}
    auto data=open_owned_directory(root/"data/clamav"),work=open_owned_directory(root/"work/clamav");auto gate=operation_gate(work.get());require(flock(gate.get(),LOCK_EX|LOCK_NB)==0,"clamav_initialization_busy");
    const bool initialized=!absent(data.get(),"initialized.v1");const std::string marker="SFX-LINUX-CLAMAV-DATA-1\n";
    if(initialized){const auto bytes=read_private_file(data.get(),"initialized.v1",128);require(std::string(bytes.begin(),bytes.end())==marker,"clamav_initialization_invalid");}
    if(absent(data.get(),"db")){require(!initialized&&names(data.get()).empty(),"clamav_initialized_database_missing");require(mkdirat(data.get(),"db",0700)==0&&fsync(data.get())==0,"clamav_database_create");}
    auto database=open_owned_directory(root/"data/clamav/db");(void)database;
    if(!initialized)publish_private_file(data.get(),"initialized.v1",{marker.begin(),marker.end()},false);
}
LinuxClamAvStatus inspect_linux_clamav(const std::filesystem::path& root,const std::function<bool()>& cancelled)noexcept{
    LinuxClamAvStatus result;
    try{
        const auto live=[&]{return !cancelled||!cancelled();};require(geteuid()!=0&&live(),"clamav_status_cancelled");auto bin=open_owned_directory(root/"bin");
        if(absent(bin.get(),"clamav")){result.diagnostic="clamav_not_installed";return result;}result.installed=true;
        auto engine=open_owned_directory(root/"bin/clamav");const auto images=pin_engine(engine.get(),live);result.verified=true;result.version=app::kClamAvEngineVersion;
        auto work=open_owned_directory(root/"work/clamav");auto gate=operation_gate(work.get());if(flock(gate.get(),LOCK_SH|LOCK_NB)!=0){require(errno==EWOULDBLOCK||errno==EAGAIN,"clamav_gate_failed");result.busy=true;result.diagnostic="clamav_status_operation_busy";return result;}
        auto data=open_owned_directory(root/"data/clamav");const auto prior=history(data.get());if(prior){result.history_available=true;result.last_attempt=prior->attempt;result.last_success=prior->success;result.last_change=prior->changed;result.last_mode=prior->mode;result.last_result=prior->result;result.change=prior->change;}
        auto database=open_owned_directory(root/"data/clamav/db");const auto pins=pin_databases(database.get(),live,true);result.definitions_present=true;
        result.definitions_verified=prior&&prior->success>0&&prior->fingerprints==fingerprints(pins);
        if(result.definitions_verified)for(std::size_t i=0;i<3;++i)result.databases[i]=database_version(pins[i]);
        result.diagnostic=result.definitions_verified?"clamav_definitions_verified":"clamav_definitions_unverified";
        if(prior&&prior->result!="clamav_definitions_verified")result.diagnostic=prior->result;
    }catch(const std::exception& error){result.diagnostic=std::string_view(error.what()).rfind("clamav_",0)==0?error.what():"clamav_status_unavailable";}catch(...){result.diagnostic="clamav_status_unavailable";}
    return result;
}
LinuxClamAvUpdateResult update_linux_clamav(const std::filesystem::path& root,std::int64_t utc,UpdateMode mode,
    const std::function<bool()>& cancelled,const std::function<void(std::uint64_t)>& activity,std::uint32_t deadline)noexcept{
    LinuxClamAvUpdateResult result;const auto began=now();FD data,work,gate;std::unique_ptr<Scratch> scratch;UpdateHistory state;std::string transcript;bool history_loaded{},published{},publication_durable{};
    try{
        require(geteuid()!=0&&utc>0&&utc<=32503680000LL&&deadline>0&&deadline<=180000&&(mode==UpdateMode::automatic||mode==UpdateMode::manual),"clamav_update_policy");
        const auto poll=[&]{if(cancelled&&cancelled()){result.cancelled=true;throw std::runtime_error("clamav_update_cancelled");}require(now()-began<deadline,"clamav_update_timeout");};
        const auto live=[&]{poll();return true;};poll();work=open_owned_directory(root/"work/clamav");gate=operation_gate(work.get());
        while(flock(gate.get(),LOCK_EX|LOCK_NB)!=0){require(errno==EAGAIN||errno==EWOULDBLOCK,"clamav_gate_failed");poll();if(activity)activity(now()-began);std::this_thread::sleep_for(std::chrono::milliseconds(25));}
        auto engine=open_owned_directory(root/"bin/clamav");auto images=pin_engine(engine.get(),live);data=open_owned_directory(root/"data/clamav");
        state=history(data.get()).value_or(UpdateHistory{});history_loaded=true;require(utc>=state.attempt,"clamav_clock_moved_backwards");
        if(mode==UpdateMode::automatic){
            if(!absent(data.get(),"update-day.v1")){const auto bytes=read_private_file(data.get(),"update-day.v1",64);const std::string text(bytes.begin(),bytes.end());require(text.size()>1&&text.back()=='\n',"clamav_update_day_invalid");
                if(utc/86400<=decimal(text.substr(0,text.size()-1))){result.diagnostic="clamav_update_already_attempted_today";return result;}}
            const auto day=std::to_string(utc/86400)+"\n";publish_private_file(data.get(),"update-day.v1",{day.begin(),day.end()},true);
        }
        result.attempted=true;state.attempt=utc;state.mode=mode;state.result="clamav_update_interrupted";save_history(data.get(),state);
        auto previous=open_owned_directory(root/"data/clamav/db");auto old=pin_databases(previous.get(),live,false);
        const bool had_definitions=std::any_of(old.begin(),old.end(),[](const auto& pin){return pin.name!="freshclam.dat";});
        bool old_verified{};std::array<std::string,3> old_hashes;try{old_hashes=fingerprints(old);old_verified=state.success>0&&state.fingerprints==old_hashes;}catch(...){poll();}
        scratch=std::make_unique<Scratch>(work.get());scratch->reserve(update_bound);
        require(mkdirat(scratch->directory.get(),"db",0700)==0,"clamav_update_database_create");FD next(openat(scratch->directory.get(),"db",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));require(bool(next),"clamav_update_database_open");
        for(const auto& pin:old)copy_pin(pin,next.get(),poll);
        const auto database_path=descriptor_path(next.get())+"/.";
        const std::string configuration="DatabaseDirectory "+database_path+"\nDatabaseMirror database.clamav.net\nDNSDatabaseInfo current.cvd.clamav.net\nTestDatabases yes\nBytecode yes\nConnectTimeout 15\nReceiveTimeout 60\nMaxAttempts 1\n";
        publish_private_file(scratch->directory.get(),"freshclam.conf",{configuration.begin(),configuration.end()},false);
        Pinned config(scratch->directory.get(),"freshclam.conf",4096,live);
        const auto pending=[&]{if(activity)activity(now()-began);};
        result.exit_code=run_vendor(1,engine.get(),images,*scratch,gate.get(),{next.get(),config.file.get()},
            {"freshclam","--config-file="+descriptor_path(config.file.get()),"--stdout"},poll,pending,result.process_reaped,transcript);
        config.verify();for(const auto& pin:old)pin.verify();
        // Preserve the vendor's retry/cooldown state even when its request fails.
        // This small data file is never treated as a successful verification.
        if(!absent(next.get(),"freshclam.dat")){Pinned vendor(next.get(),"freshclam.dat",4096,live);const auto bytes=read_private_file(next.get(),"freshclam.dat",4096);vendor.verify();publish_private_file(previous.get(),"freshclam.dat",bytes,true);}
        if(result.exit_code!=0){std::string lower=transcript;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(c>='A'&&c<='Z'?c-'A'+'a':c);});
            require(lower.find("cool-down")==std::string::npos&&lower.find("cooldown")==std::string::npos&&lower.find("429")==std::string::npos&&lower.find("403")==std::string::npos,"clamav_update_vendor_cooldown");throw std::runtime_error("clamav_update_vendor_failed");}
        auto next_pins=pin_databases(next.get(),live,true);const auto next_hashes=fingerprints(next_pins);
        // A successful FreshClam 'already current' exit need not load an existing
        // database. Independently load all three through the pinned scanner before
        // publishing or establishing a successful fingerprint receipt.
        const std::string sentinel="SFX definition verification.\n";publish_private_file(scratch->directory.get(),"verify.txt",{sentinel.begin(),sentinel.end()},false);Pinned input(scratch->directory.get(),"verify.txt",1024,live);
        std::string verification;const auto input_path=descriptor_path(input.file.get());
        const auto scan_exit=run_vendor(0,engine.get(),images,*scratch,gate.get(),{next.get(),input.file.get()},
            {"clamscan","--stdout","--allmatch","--official-db-only=yes","--fail-if-cvd-older-than=7","--database="+database_path,"--follow-dir-symlinks=0","--follow-file-symlinks=2","--",input_path},poll,pending,result.process_reaped,verification);
        app::FileScanEvidence evidence;evidence.started=true;evidence.exited=true;evidence.exit_code=scan_exit;evidence.output_complete=true;evidence.expected_output_path=input_path;evidence.standard_output=verification;
        require(app::classify_clamav_scan(evidence).verdict==app::FileScanVerdict::clean,"clamav_update_verification_failed");
        for(const auto& pin:next_pins)pin.verify();
        input.verify();config.verify();poll();
        if(!had_definitions)result.change=DefinitionChange::initial;
        else if(old_verified){result.change=next_hashes==old_hashes?DefinitionChange::current:DefinitionChange::updated;
            if(result.change==DefinitionChange::updated){bool comparable=true,equal=true;for(std::size_t i=0;i<3;++i){const auto a=database_version(old[i]),b=database_version(next_pins[i]);if(!a||!b){comparable=false;break;}equal=equal&&a->version==b->version&&a->signatures==b->signatures&&a->published==b->published;}if(comparable&&equal)result.change=DefinitionChange::current;}}
        const auto daily=database_version(next_pins[1]);if(daily)result.daily_version=daily->version;
        // Same-filesystem atomic directory exchange: readers see either complete
        // set; an interruption cannot strand a missing or mixed active db folder.
        struct stat held{},named{};require(fstat(previous.get(),&held)==0&&fstatat(data.get(),"db",&named,AT_SYMLINK_NOFOLLOW)==0&&held.st_dev==named.st_dev&&held.st_ino==named.st_ino,"clamav_update_database_replaced");
        require(fstat(next.get(),&held)==0&&fstatat(scratch->directory.get(),"db",&named,AT_SYMLINK_NOFOLLOW)==0&&held.st_dev==named.st_dev&&held.st_ino==named.st_ino,"clamav_update_staging_replaced");
        require(fsync(next.get())==0&&syscall(SYS_renameat2,scratch->directory.get(),"db",data.get(),"db",2U)==0,"clamav_update_atomic_publish");published=true;
        require(fsync(data.get())==0&&fsync(scratch->directory.get())==0,"clamav_update_publish_flush");
        publication_durable=true;
        state.success=utc;state.fingerprints=next_hashes;state.change=result.change;if(result.change==DefinitionChange::initial||result.change==DefinitionChange::updated)state.changed=utc;
        result.succeeded=true;result.diagnostic="clamav_definitions_verified";
    }catch(const std::exception& error){const std::string message=error.what();result.diagnostic=message.rfind("clamav_",0)==0&&message.size()<100?message:"clamav_update_unavailable";}
    catch(...){result.diagnostic="clamav_update_unavailable";}
    if(!result.process_reaped){result.succeeded=false;result.diagnostic="clamav_update_child_exit_unconfirmed";}
    bool recorded{};
    if(result.attempted&&history_loaded&&data&&result.process_reaped){try{state.result=result.diagnostic;save_history(data.get(),state);recorded=true;}catch(...){result.succeeded=false;result.diagnostic="clamav_update_status_unavailable";}}
    // Keep the last bounded vendor output under the portable data root. Do not
    // permit a vendor-controlled logfile pathname or terminal escape sequences.
    if(data&&result.attempted&&result.process_reaped)try{if(transcript.size()>65536)transcript.erase(0,transcript.size()-65536);publish_private_file(data.get(),"update-last.log",{transcript.begin(),transcript.end()},true);}catch(...){result.cleanup_complete=false;}
    // An exchange alone does not prove durable publication. Keep the last good
    // complete directory after any publication flush or success-receipt failure.
    if(scratch&&result.process_reaped&&(!published||(publication_durable&&result.succeeded&&recorded))){
        FD db(openat(scratch->directory.get(),"db",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        bool cleaned=db&&erase_flat(db.get())&&unlinkat(scratch->directory.get(),"db",AT_REMOVEDIR)==0;
        if(cleaned)cleaned=erase_flat(scratch->directory.get());
        result.cleanup_complete=result.cleanup_complete&&cleaned;
    }else if(scratch)result.cleanup_complete=false;
    return result;
}
}
