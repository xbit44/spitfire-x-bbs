int external_stored_fixture(Fixture& board){
    const auto& root=board.root;const auto folder=root/"Doors/Game/NetMail";std::filesystem::create_directories(folder);
#ifndef _WIN32
    for(const auto& path:{root/"Doors",root/"Doors/Game",folder})std::filesystem::permissions(path,std::filesystem::perms::owner_all);
#endif
    const std::array<const char*,3> names{{"normal.bin","hold.bin","crash.bin"}};
    for(unsigned i=0;i<3;++i){
        std::string bytes(190,'\0');bytes.replace(0,10,"BRE System");bytes.replace(36,10,"BRE System");bytes.replace(72,std::string_view(names[i]).size(),names[i]);
        const auto word=[&](std::size_t at,unsigned value){bytes[at]=static_cast<char>(value);bytes[at+1]=static_cast<char>(value>>8);};
        word(166,2);word(168,1);word(172,100);word(174,100);word(186,0x191|(i==2?2:0));bytes+="\1INTL 2:100/2 2:100/1\r\1FLAGS KFS";if(i==1)bytes+=" HLD";bytes+="\r\n";bytes.push_back('\0');
        external_fixture_write(folder/(std::to_string(i+1)+".MSG"),bytes);external_fixture_write(folder/names[i],std::string("Game payload ")+names[i]);
    }
    board.config.links.front().credentials={};board.config.links.front().poll_minutes=0;board.config.networks.front().msg_netmail_enabled=true;board.config.networks.front().msg_netmail_directory="Doors/Game/NetMail";
    board.save(2);external_start(board);
    await([&]{return external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")==3;},"normal Hold and Crash MSG attachment requests handed off");
    for(unsigned flavor=0;flavor<3;++flavor){const auto sql="SELECT count(*) FROM sfx_ftn_outbound WHERE state=5 AND flavor="+std::to_string(flavor);check(external_fixture_count(board,sql.c_str())==1,"each attachment keeps its requested mail flavor");}
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==0,"local MSG handoff has no remote delivery claim");external_fixture_write(root/"READY","MSG flavor flows ready\n");
    await([&]{return std::filesystem::exists(root/"STOCK_RECEIVED");},"driver received MSG attachments through stock");
    board.engine->stop();board.engine.reset();external_start(board);std::this_thread::sleep_for(std::chrono::seconds(2));board.engine->stop();
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound")==3&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")==3,"restart never requeues stock-consumed handoffs");
    for(unsigned i=0;i<3;++i)check(std::filesystem::exists(folder/(std::to_string(i+1)+".MSG"))&&std::filesystem::exists(folder/names[i]),"KFS and kill-sent originals retained without a trustworthy remote delivery acknowledgement");
    check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"stored message references clean");check(board.db->shutdown().succeeded(),"stored message fixture closed");external_fixture_write(root/"DONE","PASS External MSG flavors\n");std::cout<<"PASS external MSG Runtime "<<checks<<" checks\n";return 0;
}
