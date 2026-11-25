{
    // Real shipping Runtime and Panel; synthetic authenticated client, loopback only.
    struct ProgressClient {SOCKET value{INVALID_SOCKET};~ProgressClient(){if(value!=INVALID_SOCKET)closesocket(value);}} client;
    client.value=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(client.value!=INVALID_SOCKET,"progress client socket");DWORD timeout=5000;
    check(!setsockopt(client.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout))&&!setsockopt(client.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)),"bounded progress socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(26537);check(!connect(client.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"progress connects only to isolated installed listener");
    const auto send_all=[&](const std::string& bytes){std::size_t at{};while(at<bytes.size()){const auto n=send(client.value,bytes.data()+at,static_cast<int>(bytes.size()-at),0);check(n>0,"bounded progress frame sent");at+=n;}};
    const std::string filename="progress-rc15-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".bin";
    app::BinkpDecoder decoder;app::BinkpOptions options;bool adr{},accepted{},got{},ended{},skipped{};unsigned got_count{};char buffer[8192]{};
    const auto read=[&]{const auto n=recv(client.value,buffer,sizeof(buffer),0);check(n>0,"progress reply received");check(decoder.feed(std::string_view(buffer,n),[&](app::BinkpFrame frame){if(!frame.command)return true;const auto command=static_cast<app::BinkpCommand>(frame.id);
        if(command==app::BinkpCommand::nul)app::observe_binkp_information(options,frame.bytes,true);
        if(command==app::BinkpCommand::adr)adr=true;if(command==app::BinkpCommand::ok)accepted=true;if(command==app::BinkpCommand::eob)ended=true;
        if(command==app::BinkpCommand::got){const auto file=app::parse_binkp_file(frame.bytes,false,false);check(file&&file->name==filename&&file->size==262144,"exact synthetic file acknowledged");++got_count;got=true;}
        if(command==app::BinkpCommand::skip){const auto file=app::parse_binkp_file(frame.bytes,false,false);check(file&&file->name==filename,"exact conflicting original filename skipped");skipped=true;}
        check(command!=app::BinkpCommand::err&&command!=app::BinkpCommand::bsy,"actual progress session not refused");return true;}),"actual progress frames decoded");SecureZeroMemory(buffer,sizeof(buffer));};
    for(unsigned i=0;i<20&&!adr;++i)read();check(adr&&!options.challenge_hex.empty(),"real challenge available");
    send_all(app::binkp_command(app::BinkpCommand::nul,"VER SyntheticProgress/1.0 (binkp/1.0)"));send_all(app::binkp_command(app::BinkpCommand::adr,"42:100/2@own-test"));
    auto response=win32::ftn_cram_response("OnlySession123",options.challenge_hex);check(response.has_value(),"synthetic credential response computed");send_all(app::binkp_command(app::BinkpCommand::pwd,response->view()));response.reset();
    for(unsigned i=0;i<20&&!accepted;++i)read();check(accepted,"real installed handshake accepted");
    send_all(app::binkp_command(app::BinkpCommand::file,filename+" 262144 1700000100 0"));
    for(unsigned chunk=0;chunk<64;++chunk){send_all(app::binkp_data(std::string(4096,static_cast<char>('a'+chunk%26))));Sleep(120);
        if(chunk==31){const auto until=GetTickCount64()+5000;std::wstring progress;
            const auto read_progress=[&]{progress.clear();EnumChildWindows(panel.window,[](HWND h,LPARAM value)->BOOL{auto& text=*reinterpret_cast<std::wstring*>(value);const auto candidate=foreign_text(h);if(IsWindowVisible(h)&&candidate.find(L"RECEIVE - progress-")==0){text=candidate;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&progress));};
            do{read_progress();if(progress.find(L"/s")!=std::wstring::npos)break;Sleep(30);}while(GetTickCount64()<until);
            std::wcout<<L"MIDPOINT UI "<<progress<<L"\n";render_foreign_png(panel.window,output/L"binkp-live-receive.png");
            check(visible_text_contains(panel.window,L"Follow active session - Owner fix regression / Synthetic peer"),"shipping Panel follows new active connection after old failures");
            const auto newline=progress.find(L'\n');const auto position=newline==progress.npos?0:std::stoull(progress.substr(newline+1));
            check(position>0&&position<=131072&&progress.find(L" / 262144 bytes")!=progress.npos&&progress.find(L"/s")!=progress.npos,"real sub-256KiB progress and speed reach shipping UI through Runtime IPC");}

    }
    for(unsigned i=0;i<20&&!got;++i)read();check(got,"real durable receipt ACK observed");
    // Hold an ordinary inbound reader across the duplicate offer. Before the
    // RC10 correction the unnecessary recovery DELETE open failed with Win32 32.
    auto completed_directory=win32::FilePayloadDirectory::private_store(root/L"FTN"/L"Inbound",owner);
    auto original_reader=completed_directory.read(filename);const auto original_identity=original_reader.identity();
    send_all(app::binkp_command(app::BinkpCommand::file,filename+" 262144 1700000100 0"));for(unsigned i=0;i<20&&!skipped;++i)read();check(skipped&&got_count==1,"shipping Runtime skips reader-held duplicate without a second GOT");
    check(original_reader.identity().file_id==original_identity.file_id&&original_reader.identity().sha256==original_identity.sha256,"reader-held original object and complete bytes unchanged after SKIP");
    send_all(app::binkp_command(app::BinkpCommand::eob));for(unsigned i=0;i<20&&!ended;++i)read();check(ended,"peer batch ended");
    shutdown(client.value,SD_SEND);const auto until=GetTickCount64()+10000;while(!visible_text_contains(panel.window,L"100%   Session completed")&&GetTickCount64()<until)Sleep(30);
    check(visible_text_contains(panel.window,L"262144 / 262144 bytes   100%   Session completed"),"actual completion retains final bytes and complete state");render_foreign_png(panel.window,output/L"binkp-receive-completed.png");
    {const auto incoming=root/L"FTN"/L"Inbound"/std::filesystem::u8path(filename);check(std::filesystem::exists(incoming)&&std::filesystem::file_size(incoming)==262144,"shipping Runtime publishes original filename directly in flat Inbound");std::vector<std::uint8_t> payload_expected;for(unsigned chunk=0;chunk<64;++chunk)payload_expected.insert(payload_expected.end(),4096,static_cast<std::uint8_t>('a'+chunk%26));check(win32::installation::file_sha256(incoming)==win32::installation::sha256(payload_expected),"shipping inbound payload matches every transmitted byte");for(const auto& entry:std::filesystem::directory_iterator(root/L"FTN"/L"Inbound"))check(!entry.is_directory(),"shipping legacy migration leaves no ID folders in Inbound");const auto log_bytes=win32::installation::read_file(root/L"Logs"/L"bink.log",1048576);const std::string log(log_bytes.begin(),log_bytes.end());check(log.find(filename)!=log.npos&&log.find("Session completed")!=log.npos&&log.find("OnlySession123")==log.npos&&log.find("CRAM-MD5-")==log.npos,"shipping persistent bink.log records transfer/results without credentials");}
}
