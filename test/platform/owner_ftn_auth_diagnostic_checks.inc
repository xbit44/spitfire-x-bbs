{
    // Real installed Runtime, synthetic configured peer, loopback only.
    // Deliberately malformed responses contain no credential or digest.
    const auto reject=[&](const std::string& response,const std::string& expected){
        struct Client { SOCKET value{INVALID_SOCKET};~Client(){if(value!=INVALID_SOCKET)closesocket(value);} } client;
        client.value=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(client.value!=INVALID_SOCKET,"diagnostic loopback client created");
        DWORD timeout=5000;check(!setsockopt(client.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)),"bounded diagnostic receive");
        check(!setsockopt(client.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)),"bounded diagnostic send");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(24777);
        check(!connect(client.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"connect to isolated installed BinkP listener only");
        const auto send_all=[&](const std::string& bytes){std::size_t sent{};while(sent<bytes.size()){const auto n=send(client.value,bytes.data()+sent,static_cast<int>(bytes.size()-sent),0);check(n>0,"send synthetic diagnostic frame");sent+=static_cast<std::size_t>(n);}};
        app::BinkpDecoder decoder;bool advertised{},cram{};std::string error;char buffer[4096]{};
        const auto read=[&]{const auto n=recv(client.value,buffer,sizeof(buffer),0);check(n>0,"bounded actual diagnostic reply");check(decoder.feed(std::string_view(buffer,static_cast<std::size_t>(n)),[&](app::BinkpFrame frame){
            if(frame.command&&frame.id==static_cast<unsigned>(app::BinkpCommand::adr))advertised=true;
            if(frame.command&&frame.id==static_cast<unsigned>(app::BinkpCommand::nul)&&frame.bytes.find("OPT CRAM-MD5-")==0)cram=true;
            if(frame.command&&frame.id==static_cast<unsigned>(app::BinkpCommand::err))error=frame.bytes;
            check(!(frame.command&&frame.id==static_cast<unsigned>(app::BinkpCommand::ok)),"malformed diagnostic never receives acceptance");return true;}),"decode diagnostic reply without recording transcript");SecureZeroMemory(buffer,sizeof(buffer));};
        for(unsigned i=0;i<20&&!advertised;++i)read();check(advertised&&cram,"actual installed listener advertises CRAM before synthetic response");
        send_all(app::binkp_command(app::BinkpCommand::adr,"42:100/2@own-test"));send_all(app::binkp_command(app::BinkpCommand::pwd,response));
        for(unsigned i=0;i<20&&error.empty();++i)read();check(error==expected,"shipping Runtime classifies malformed CRAM precisely");
    };
    reject("CRAM-MD5-"+std::string(32,'g'),"ftn_session_cram_response_encoding");
    reject("CRAM-MD5-"+std::string(31,'0'),"ftn_session_cram_response_length");
    const auto deadline=GetTickCount64()+15000;
    while(!visible_text_contains(panel.window,L"CRAM response has an unexpected length")&&GetTickCount64()<deadline)Sleep(30);
    check(visible_text_contains(panel.window,L"Address matched configured connection; authentication pending.")&&visible_text_contains(panel.window,L"CRAM response is not a valid hexadecimal digest")&&visible_text_contains(panel.window,L"CRAM response has an unexpected length"),"shipping Panel shows candidate and both classified auth failures");
    check(visible_text_contains(panel.window,L"Owner fix regression / Synthetic peer")&&!visible_text_contains(panel.window,utf8_to_wide(original.network))&&!visible_text_contains(panel.window,utf8_to_wide(original.code)),"actual auth diagnostics resolve friendly names and hide routing IDs");
    check(!visible_text_contains(panel.window,L"OnlySession123")&&!visible_text_contains(panel.window,L"OnlyAreaFix123"),"actual Panel diagnostic contains no configured passwords");
    render_foreign_png(panel.window,output/L"binkp-auth-diagnostic.png");
}
