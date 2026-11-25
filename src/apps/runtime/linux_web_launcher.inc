// Native launcher: no shell command construction, no browser password, and no
// BBS lifecycle command. The private owner credential is never given to xdg-open.
class BrowserLaunchError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::optional<std::string> local_launch_url(const std::filesystem::path& root){
    try{
        const auto directory=host::open_owned_directory(root/"management");
        const auto bytes=host::read_private_file(directory.get(),"web-launcher.v1",512);
        auto record=Json::parse(bytes.begin(),bytes.end());require_keys(record,{"version","port","owner"});
        if(record.at("version")!=1||!record.at("port").is_number_integer())return {};
        const auto port=record.at("port").get<int>();if(port<1024||port>65535)return {};
        const auto owner=record.at("owner").get<std::string>();if(owner.size()!=64)return {};
        const auto origin="http://127.0.0.1:"+std::to_string(port);
        httplib::Client client("127.0.0.1",port);client.set_connection_timeout(1);client.set_read_timeout(2);client.set_write_timeout(2);
        const auto response=client.Post("/api/local-launch",{{"Origin",origin}},Json{{"owner",owner}}.dump(),"application/json");
        if(!response||response->status!=200||response->body.size()>256)return {};
        const auto value=Json::parse(response->body);require_keys(value,{"token"});const auto token=value.at("token").get<std::string>();
        if(token.size()!=64||token.find_first_not_of("0123456789abcdef")!=std::string::npos)return {};
        return origin+"/#launch="+token;
    }catch(const std::exception&){return {};}
}
int launch_browser(const std::filesystem::path& root,const std::filesystem::path& tzdata,bool print_url){
    if(geteuid()==0||!root.is_absolute()||!tzdata.is_absolute())throw std::runtime_error("launcher_context_invalid");
    host::layout::require_current(root);
    const auto root_pin=host::open_owned_directory(root);
    if(mkdirat(root_pin.get(),"management",0700)!=0&&errno!=EEXIST)throw std::runtime_error("launcher_management_unavailable");
    host::InstallationLock launch_lock;
    if(!launch_lock.acquire(root/"management","web-launcher.lock"))throw std::runtime_error("another_launcher_is_opening");
    auto url=local_launch_url(root);
    if(!url){
        const auto directory=host::open_owned_directory(root/"management");
        // Open and validate the exact log inode before forking. Never follow a
        // substituted link or append to another account's file.
        host::FileDescriptor log(openat(directory.get(),"web-start.log",O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600));
        if(!log||!host::private_regular_file(log.get(),16U*1024U*1024U))throw std::runtime_error("launcher_log_unavailable");
        const auto executable=std::filesystem::read_symlink("/proc/self/exe").string();
        const auto root_text=root.string(),zone_text=tzdata.string();
        const auto child=fork();if(child<0)throw std::runtime_error("launcher_start_failed");
        if(child==0){
            if(setsid()<0)_exit(126);
            const int null=open("/dev/null",O_RDONLY|O_CLOEXEC);if(null<0||dup2(null,STDIN_FILENO)<0||dup2(log.get(),STDOUT_FILENO)<0||dup2(log.get(),STDERR_FILENO)<0)_exit(126);
            close(null);
            execl(executable.c_str(),executable.c_str(),"--root",root_text.c_str(),"--tzdata",zone_text.c_str(),"--port","0",static_cast<char*>(nullptr));_exit(127);
        }
        // A second owner cannot take over a healthy running board. A slow or
        // failed launch is reported; the launcher never kills/restarts Runtime.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!url&&std::chrono::steady_clock::now()<deadline){
            int status{};const auto ended=waitpid(child,&status,WNOHANG);
            if(ended==child)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));url=local_launch_url(root);
        }
        if(!url)throw std::runtime_error("launcher_web_not_ready");
    }
    launch_lock.release();
    if(print_url){std::cout<<*url<<'\n';return 0;}
    const auto directory=host::open_owned_directory(root/"management");
    host::FileDescriptor browser_log(openat(directory.get(),"browser-launch.log",
        O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600));
    if(!browser_log || !host::private_regular_file(browser_log.get(),UINT64_MAX))
        throw BrowserLaunchError("Cannot open the private management/browser-launch.log. Use spitfire-x --print-url for a launch link.");
    if(ftruncate(browser_log.get(),0)!=0)
        throw BrowserLaunchError("Cannot reset management/browser-launch.log. Use spitfire-x --print-url for a launch link.");
    const auto browser=fork();
    if(browser<0)throw BrowserLaunchError("Cannot start the desktop browser. Use spitfire-x --print-url for a launch link.");
    if(browser==0){
        if(!host::drop_child_capabilities() || dup2(browser_log.get(),STDOUT_FILENO)<0 ||
            dup2(browser_log.get(),STDERR_FILENO)<0)_exit(126);
        execlp("xdg-open","xdg-open",url->c_str(),static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};
    while(waitpid(browser,&status,0)<0){
        if(errno!=EINTR)throw BrowserLaunchError("Cannot verify desktop browser launch. See management/browser-launch.log.");
    }
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){
        const auto detail=WIFEXITED(status)?"exit code "+std::to_string(WEXITSTATUS(status)):
            "signal "+std::to_string(WTERMSIG(status));
        throw BrowserLaunchError("The desktop browser could not open ("+detail+"). "
            "See management/browser-launch.log or run spitfire-x --print-url for a launch link.");
    }
    return 0;
}
int stop_management(const std::filesystem::path& root){
    if(geteuid()==0||!root.is_absolute())throw std::runtime_error("launcher_context_invalid");
    host::layout::require_current(root);
    const auto root_pin=host::open_owned_directory(root);
    const auto url=local_launch_url(root);
    if(!url){
        host::InstallationLock stopped;if(stopped.acquire(root,".sfx-runtime.lock")){std::cout<<"Management is already closed.\n";return 0;}
        throw std::runtime_error("Management could not be reached. No process was stopped.");
    }
    const auto fragment=url->find("/#launch=");if(fragment==std::string::npos)throw std::runtime_error("launcher_link_invalid");
    const auto origin=url->substr(0,fragment),token=url->substr(fragment+9);
    httplib::Client client(origin);client.set_connection_timeout(2);client.set_read_timeout(5);client.set_write_timeout(2);
    const auto session=client.Post("/api/launch",{{"Origin",origin}},Json{{"token",token}}.dump(),"application/json");
    if(!session||session->status!=200)throw std::runtime_error("Management authorization failed. Nothing was stopped.");
    const auto body=Json::parse(session->body);const auto csrf=body.at("csrf").get<std::string>();
    const auto cookie_value=session->get_header_value("Set-Cookie");const auto credential=cookie_value.substr(0,cookie_value.find(';'));
    const auto result=client.Post("/api/management/close",{{"Origin",origin},{"Cookie",credential},{"X-SFX-CSRF",csrf}},"{}","application/json");
    if(!result)throw std::runtime_error("The close result is unavailable. Check again before updating.");
    const auto closed=Json::parse(result->body);
    if(result->status!=200){std::cerr<<closed.value("detail","Management could not close.")<<'\n';return 1;}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<deadline){host::InstallationLock stopped;if(stopped.acquire(root,".sfx-runtime.lock")){std::cout<<"Management is closed. You can update this installation.\n";return 0;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}
    throw std::runtime_error("Management has not released this installation yet. Wait, then check again before updating.");
}
