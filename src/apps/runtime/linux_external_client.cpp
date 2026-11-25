#include "linux_external_client.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <charconv>
#include <map>
#include <set>

extern char** environ;
namespace sfx::apps::runtime::web {
namespace {
namespace native=sfx::platform::linux_host;
using Json=nlohmann::json;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void keys(const Json& value,std::initializer_list<const char*> names){
    require(value.is_object()&&value.size()==names.size(),"desktop_fields_invalid");
    for(const auto* name:names)require(value.contains(name),"desktop_fields_invalid");
}
bool printable(const std::string& text){for(const unsigned char c:text)if(c<32||c==127)return false;return true;}
std::string expand(const std::string& token,std::uint16_t port,unsigned& hosts,unsigned& ports){
    std::string out;
    for(std::size_t i=0;i<token.size();){
        if(token.compare(i,6,"{HOST}")==0){out+="127.0.0.1";i+=6;++hosts;}
        else if(token.compare(i,6,"{PORT}")==0){out+=std::to_string(port);i+=6;++ports;}
        else{require(token[i]!='{'&&token[i]!='}',"desktop_placeholder_invalid");out+=token[i++];}
    }
    return out;
}
}
DesktopClientProfile parse_desktop_client(const Json& value){
    keys(value,{"revision","type","executable","arguments"});DesktopClientProfile result;
    require(value.at("revision").is_string()&&value.at("type").is_number_integer()&&value.at("type")>=0&&value.at("type")<=2&&
        value.at("executable").is_string()&&value.at("arguments").is_array(),"desktop_value_invalid");
    const auto text=value.at("revision").get<std::string>();const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result.revision);
    require(parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&result.revision&&result.revision<UINT64_MAX&&std::to_string(result.revision)==text,"desktop_revision_invalid");
    result.type=value.at("type").get<unsigned>();result.executable=value.at("executable").get<std::string>();result.arguments=value.at("arguments").get<std::vector<std::string>>();
    if(!result.type){require(result.executable.empty()&&result.arguments.empty(),"desktop_clear_invalid");return result;}
    const std::filesystem::path path(result.executable);
    require(!result.executable.empty()&&result.executable.size()<=3072&&printable(result.executable)&&path.is_absolute()&&path.lexically_normal()==path,"desktop_path_invalid");
    if(result.type==1)require(path.filename()=="syncterm"&&result.arguments.empty(),"desktop_syncterm_invalid");
    else{
        require(!result.arguments.empty()&&result.arguments.size()<=8,"desktop_argument_count_invalid");unsigned hosts{},ports{};
        for(const auto& token:result.arguments){require(!token.empty()&&token.size()<=512&&printable(token),"desktop_argument_invalid");(void)expand(token,65535,hosts,ports);}
        require(hosts==1&&ports==1,"desktop_one_host_and_port_required");
    }
    // Includes argv[0], terminators and maximum expanded port; no shell quoting.
    std::size_t total=result.executable.size()+1;
    if(result.type==1)total+=3+std::string("telnet://127.0.0.1:65535").size()+1;
    else{unsigned hosts{},ports{};for(const auto& token:result.arguments)total+=expand(token,65535,hosts,ports).size()+1;}
    require(total<=4096,"desktop_command_too_long");return result;
}
Json desktop_client_json(const DesktopClientProfile& p){return {{"revision",std::to_string(p.revision)},{"type",p.type},{"executable",p.executable},{"arguments",p.arguments}};}
DesktopClientProfile read_desktop_client(const std::filesystem::path& root){
    const auto directory=native::open_owned_directory(root/"config");struct stat info{};
    if(fstatat(directory.get(),"desktop-client-linux.v1",&info,AT_SYMLINK_NOFOLLOW)!=0){require(errno==ENOENT,"desktop_profile_unreadable");return {};}
    const auto bytes=native::read_private_file(directory.get(),"desktop-client-linux.v1",16384);std::map<int,std::set<std::string>> fields;
    return parse_desktop_client(Json::parse(bytes.begin(),bytes.end(),[&](int depth,Json::parse_event_t event,Json& value){
        require(depth<=3,"desktop_profile_depth_invalid");if(event==Json::parse_event_t::object_start)fields[depth+1].clear();
        if(event==Json::parse_event_t::key)require(fields[depth].insert(value.get<std::string>()).second,"desktop_profile_duplicate_field");
        return true;
    }));
}
void write_desktop_client(const std::filesystem::path& root,const DesktopClientProfile& value,std::uint64_t expected){
    native::InstallationLock lock;require(lock.acquire(root/"management","desktop-client-linux.lock"),"desktop_profile_busy");
    const auto checked=parse_desktop_client(desktop_client_json(value));require(expected<UINT64_MAX-1&&checked.revision==expected+1,"desktop_revision_invalid");
    require(read_desktop_client(root).revision==expected,"desktop_profile_changed");if(checked.type)(void)pin_desktop_client(checked);
    const auto directory=native::open_owned_directory(root/"config");const auto text=desktop_client_json(checked).dump()+"\n";
    native::publish_private_file(directory.get(),"desktop-client-linux.v1",{text.begin(),text.end()},true);
}
DesktopClientProfile desktop_client_update(const Json& request){
    keys(request,{"revision","values"});const auto& values=request.at("values");keys(values,{"type","executable","arguments"});
    const auto text=values.at("arguments").get<std::string>();require(text.size()<=4096,"desktop_arguments_too_long");std::vector<std::string> arguments;
    if(!text.empty()){std::size_t start{};for(;;){const auto end=text.find('\n',start);arguments.push_back(text.substr(start,end==text.npos?text.npos:end-start));if(end==text.npos)break;start=end+1;}}
    auto result=parse_desktop_client({{"revision",request.at("revision")},{"type",values.at("type")},{"executable",values.at("executable")},{"arguments",arguments}});
    require(result.revision<UINT64_MAX-1,"desktop_revision_exhausted");++result.revision;return result;
}
Json desktop_client_page(const DesktopClientProfile& profile,bool local,const std::filesystem::path& root,const std::filesystem::path& executable){
    std::string arguments;for(const auto& token:profile.arguments){if(!arguments.empty())arguments+='\n';arguments+=token;}
    const auto fields=Json::array({
        {{"key","type"},{"label","Desktop client"},{"type","select"},{"value",profile.type},{"choices",Json::array({{{"value",0},{"label","None"}},{{"value",1},{"label","SyncTERM"}},{{"value",2},{"label","Custom"}}})}},
        {{"key","executable"},{"label","Program on the Linux computer"},{"type","text"},{"maximum",3072},{"value",profile.executable},{"help","Full path to an installed executable. Saving never runs it. Use the same Linux account that owns this SFX folder."}},
        {{"key","arguments"},{"label","Custom arguments (one per line)"},{"type","multiline"},{"maximum",4096},{"value",arguments},{"help","Custom only: 1-8 arguments, up to 512 bytes each. Include {HOST} and {PORT} exactly once. Spaces inside a line stay inside that argument. No shell expansion. Leave blank for SyncTERM."}}
    });
    const auto quote=[](const std::string& text){std::string out="'";for(const char c:text)out+=c=='\''?"'\\''":std::string(1,c);return out+'\'';};
    const auto command=local&&!root.empty()&&!executable.empty()?quote(executable.string())+" --local-client --root "+quote(root.string())+" --node 2":"";
    return {{"route","connections.external_client"},{"title","External Client"},{"category","Connections"},{"revision",std::to_string(profile.revision)},
        {"desktop_command",command},
        {"fields",fields},{"active_fields",fields},{"can_save",true},{"desktop_client",true},
        {"activation",local?"Optional client for this Linux account. From a terminal in its desktop, run bin/sfxweb --local-client --root /full/path/to/your/sfx-folder --node 2 (choose the wanted node). Management and BBS must already be running. Logon in the web Control Panel always uses the browser terminal. Clear / Reset saves an empty profile; program files remain.":"This profile belongs to the Linux account that owns the SFX folder. You can save its settings here; saving never starts a program. To use it, open the local launcher on the Linux desktop with that account and follow its command. Browser Logon uses the built-in terminal and cannot start a workstation program."}};
}
PinnedDesktopClient pin_desktop_client(const DesktopClientProfile& profile){
    (void)parse_desktop_client(desktop_client_json(profile));require(profile.type!=0,"desktop_profile_not_set");
    // Resolve installed symlinks once, then pin the resulting inode. Revalidation
    // after reservation catches a changed link or in-place program replacement.
    const auto path=std::filesystem::canonical(profile.executable);PinnedDesktopClient result;
    result.file=native::FileDescriptor(open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));struct stat info{};
    require(result.file&&fstat(result.file.get(),&info)==0&&S_ISREG(info.st_mode)&&info.st_size>0&&info.st_size<=128*1024*1024&&
        (info.st_uid==0||info.st_uid==geteuid())&&(info.st_mode&0022)==0&&(info.st_mode&06000)==0&&access(path.c_str(),X_OK)==0,"desktop_program_unusable");
    result.device=static_cast<std::uint64_t>(info.st_dev);result.inode=static_cast<std::uint64_t>(info.st_ino);
    crypto_hash_sha256_state hash;require(crypto_hash_sha256_init(&hash)==0,"desktop_program_hash_failed");std::array<unsigned char,16384> buffer{};off_t offset{};
    while(offset<info.st_size){const auto size=pread(result.file.get(),buffer.data(),std::min<std::size_t>(buffer.size(),static_cast<std::size_t>(info.st_size-offset)),offset);
        if(size<0&&errno==EINTR)continue;
        require(size>0,"desktop_program_read_failed");if(!offset)result.script=size>=2&&buffer[0]=='#'&&buffer[1]=='!';
        require(crypto_hash_sha256_update(&hash,buffer.data(),static_cast<unsigned long long>(size))==0,"desktop_program_hash_failed");offset+=size;}
    require(crypto_hash_sha256_final(&hash,result.digest.data())==0,"desktop_program_hash_failed");return result;
}
bool same_desktop_client(const PinnedDesktopClient& a,const PinnedDesktopClient& b) noexcept{return a.device==b.device&&a.inode==b.inode&&a.digest==b.digest&&a.script==b.script;}
std::vector<std::string> desktop_client_arguments(const DesktopClientProfile& profile,std::uint16_t port){
    (void)parse_desktop_client(desktop_client_json(profile));require(profile.type&&port>=1024,"desktop_endpoint_invalid");
    if(profile.type==1)return {"-4","telnet://127.0.0.1:"+std::to_string(port)};
    std::vector<std::string> out;unsigned hosts{},ports{};for(const auto& token:profile.arguments)out.push_back(expand(token,port,hosts,ports));return out;
}
int spawn_desktop_client(const PinnedDesktopClient& pinned,const DesktopClientProfile& profile,std::uint16_t port){
    auto arguments=desktop_client_arguments(profile,port);arguments.insert(arguments.begin(),profile.executable);std::vector<char*> argv;
    for(auto& argument:arguments)argv.push_back(argument.data());
    argv.push_back(nullptr);
    int pipe[2];require(pipe2(pipe,O_CLOEXEC)==0,"desktop_exec_pipe_failed");native::FileDescriptor input(pipe[0]),output(pipe[1]);
    const auto child=fork();require(child>=0,"desktop_fork_failed");
    if(!child){
        close(input.get());
        // A shebang interpreter needs this FD after exec (fexecve(3)); ELF
        // clients do not inherit it. Never fall back to shell command text.
        if(native::drop_child_capabilities()&&(!pinned.script||fcntl(pinned.file.get(),F_SETFD,0)==0))fexecve(pinned.file.get(),argv.data(),environ);
        const int error=errno;const auto ignored=write(output.get(),&error,sizeof(error));(void)ignored;_exit(127);
    }
    output=native::FileDescriptor{};int error{};ssize_t count;
    do{count=read(input.get(),&error,sizeof(error));}while(count<0&&errno==EINTR);
    if(count!=0){int status{};while(waitpid(child,&status,0)<0&&errno==EINTR){}throw std::runtime_error("desktop_exec_failed");}
    return child;
}
} // namespace sfx::apps::runtime::web
