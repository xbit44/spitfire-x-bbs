    {
        const auto logroot=root/L"log-rotation";auto log_bindings=make_board(logroot,false);
        auto logs=native::FilePayloadDirectory::private_store(logroot/L"Logs",log_bindings);
        for(const auto* name:{"bink.log","bink.log.1","bink.log.2"}){auto file=logs.create(name,1048576);std::string bytes(name==std::string("bink.log")?1048570:10,'x');for(std::size_t at=0;at<bytes.size();at+=65536)file.append(reinterpret_cast<const std::uint8_t*>(bytes.data()+at),std::min<std::size_t>(65536,bytes.size()-at));file.flush();}
        runtime::append_linux_bink_log(logroot,log_bindings,1700000000,17,"Test Network","Test Peer","Session completed: 1 sent, 0 received.");
        {auto file=logs.read("bink.log");const auto bytes=file.read(0);const std::string text(bytes.begin(),bytes.end());check(text.find("session=17")!=text.npos&&text.find("Session completed")!=text.npos,"persistent BinkP log records readable timestamped transfer result");}
        check(logs.read("bink.log.1").byte_count()==1048570&&logs.read("bink.log.2").byte_count()==10,"bounded rotation retains prior generations without overwriting active log");
        runtime::append_linux_bink_log(logroot,log_bindings,1700000001,18,"Test\nNetwork","Peer\rName","New\nconnection");
        {auto file=logs.read("bink.log");const auto bytes=file.read(0);const std::string text(bytes.begin(),bytes.end());check(text.find("Test Network / Peer Name  New connection")!=text.npos&&std::count(text.begin(),text.end(),'\n')==2,"reopen appends persistently and peer text cannot inject log records");}
    }
