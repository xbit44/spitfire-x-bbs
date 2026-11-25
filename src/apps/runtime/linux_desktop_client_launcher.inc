// Invoked by the operator in a Linux desktop terminal, never by an HTTP
// handler or service startup. The saved profile belongs to this board's UID.
int launch_desktop_client(const std::filesystem::path& root,std::uint16_t node){
    if(getuid()==0||getuid()!=geteuid()||getgid()!=getegid()||!root.is_absolute()||node<1||node>app::kAlphaNodePoolCapacity||sodium_init()<0)
        throw std::runtime_error("desktop_context_invalid");
    const auto* display=getenv("DISPLAY");const auto* wayland=getenv("WAYLAND_DISPLAY");
    if(((!display||!*display)&&(!wayland||!*wayland))||getenv("SSH_CONNECTION")||getenv("SSH_TTY")){
        std::cerr<<"Open a terminal on the Linux desktop using the account that owns this SFX folder. Use browser Logon from another computer.\n";return 1;
    }
    const auto directory=host::open_owned_directory(root/"management");host::InstallationLock profile_lock;
    if(!profile_lock.acquire(root/"management","desktop-client-linux.lock"))throw std::runtime_error("desktop_profile_busy");
    const auto profile=rt::web::read_desktop_client(root);const auto pinned=rt::web::pin_desktop_client(profile);
    const auto bytes=host::read_private_file(directory.get(),"web-launcher.v1",512);auto record=Json::parse(bytes.begin(),bytes.end());require_keys(record,{"version","port","owner"});
    if(record.at("version")!=1||!record.at("port").is_number_integer())throw std::runtime_error("desktop_management_invalid");
    const auto port=record.at("port").get<int>();auto owner=record.at("owner").get<std::string>();
    if(port<1024||port>65535||owner.size()!=64||owner.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("desktop_management_invalid");
    record.clear();const auto origin="http://127.0.0.1:"+std::to_string(port);
    httplib::Client client("127.0.0.1",port);client.set_connection_timeout(1);client.set_read_timeout(5);client.set_write_timeout(2);
    const auto call=[&](const char* path,Json value){
        value["owner"]=owner;auto body=value.dump();auto result=client.Post(path,{{"Origin",origin}},body,"application/json");sodium_memzero(body.data(),body.size());value.clear();return result;
    };
    const auto result=call("/api/desktop-node/reserve",{{"node",node}});
    if(!result||result->status!=200||result->body.size()>1024){
        std::cerr<<"Desktop logon was not confirmed. Check that this node is READY and the BBS is running. An unanswered reservation expires within 30 seconds; this request will not be repeated.\n";return 1;
    }
    const auto reservation=Json::parse(result->body);require_keys(reservation,{"epoch","node","generation","port","timeout_milliseconds"});
    const auto epoch=reservation.at("epoch").get<std::string>();
    const auto generation=number(reservation.at("generation").get<std::string>());
    if(epoch.size()!=32||epoch.find_first_not_of("0123456789abcdef")!=std::string::npos||reservation.at("node")!=node||!generation||!reservation.at("port").is_number_integer()||reservation.at("port")<1024||reservation.at("port")>65535||reservation.at("timeout_milliseconds")!=30000)
        throw std::runtime_error("desktop_reservation_invalid");
    const auto cancel=[&]{try{(void)call("/api/desktop-node/cancel",{{"epoch",epoch},{"node",node},{"generation",std::to_string(generation)}});}catch(const std::exception&){}};
    int child{};
    try{
        const auto current=rt::web::read_desktop_client(root);const auto checked=rt::web::pin_desktop_client(current);
        if(rt::web::desktop_client_json(current)!=rt::web::desktop_client_json(profile)||!rt::web::same_desktop_client(pinned,checked))throw std::runtime_error("desktop_program_changed");
        child=rt::web::spawn_desktop_client(pinned,profile,reservation.at("port").get<std::uint16_t>());
    }catch(...){cancel();throw;}
    profile_lock.release();std::cout<<"Desktop client opened for node "<<node<<". Sign in with your normal caller account.\n"<<std::flush;
    int status{};pid_t ended;do{ended=waitpid(child,&status,0);}while(ended<0&&errno==EINTR);
    // This can release only a still-pending lease. It never disconnects a
    // connected client or a successor which reused the same node number.
    cancel();sodium_memzero(owner.data(),owner.size());
    if(ended!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=0){std::cerr<<"Desktop client exited with an error. Check its settings.\n";return 1;}
    return 0;
}
