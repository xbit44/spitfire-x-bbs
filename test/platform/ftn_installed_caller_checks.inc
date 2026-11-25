// Fixed installed Runtime/owner IPC control for an independent loopback caller.
// Only ordinary status/mirror and shutdown operations are exposed here.
int installed_caller_fixture(){
    check(!win32::installation::elevated()&&win32::alpha_installation_root()==std::filesystem::path(L"C:/Program Files/SFX-Section14-Test-20261030-r4"),"ordinary exact r4 caller fixture only");
    RuntimeClient runtime;std::string why;
    struct Stop{RuntimeClient& r;~Stop(){std::string reason;if(r.child_alive())r.shutdown(reason);}}stop{runtime};
    check(runtime.launch(why),"actual installed shipping Runtime launched for callers");
    const auto initial=runtime.status();check(initial&&initial->bootstrap_complete&&initial->configuration_loaded,"current installed bootstrap remains valid");
    std::cout<<"READY"<<std::endl;
    std::string command;unsigned commands{};
    while(std::getline(std::cin,command)){
        check(++commands<=256&&command.size()<=16,"bounded diagnostic control input");
        if(command=="stop")break;
        check(command=="screen","only fixed selected-node snapshot command");
        const auto status=runtime.status(1);check(status.has_value(),"actual protected status snapshot");
        const auto node=std::find_if(status->nodes.begin(),status->nodes.end(),[](const auto& value){return value.number==1;});
        check(node!=status->nodes.end()&&node->screen_included,"actual selected-node operator mirror included");
        std::string hex;constexpr char digits[]="0123456789abcdef";
        for(const auto& cell:node->screen.cells){const auto value=static_cast<unsigned char>(cell.character);hex+=digits[value>>4];hex+=digits[value&15];hex+=digits[cell.attribute>>4];hex+=digits[cell.attribute&15];}
        std::cout<<"SCREEN "<<node->terminal_columns<<' '<<node->terminal_rows<<' '<<hex<<std::endl;
    }
    check(runtime.shutdown(why),"actual installed caller Runtime normally stopped");
    std::cout<<"STOPPED"<<std::endl;return 0;
}
