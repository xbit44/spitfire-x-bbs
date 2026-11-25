// Actual game-created local .MSG -> joined scanner -> BinkP GOT -> original
// cleanup. The peer is a second isolated SFX engine, not an external hub.
{
    make_directory(client.root/"Doors");make_directory(client.root/"Doors/Game");make_directory(client.root/"Doors/Game/NetMail");
    const auto folder=client.root/"Doors/Game/NetMail";const std::string body="Local game attachment through actual joined engine and BinkP.";
    std::string message_bytes(190,'\0');message_bytes.replace(0,10,"BRE System");message_bytes.replace(36,10,"BRE System");message_bytes.replace(72,15,"stored-game.bin");
    const auto word=[&](std::size_t at,unsigned value){message_bytes[at]=static_cast<char>(value);message_bytes[at+1]=static_cast<char>(value>>8);};
    word(166,1);word(168,2);word(172,100);word(174,100);word(186,0x191);message_bytes+="\1INTL 2:100/1 2:100/2\r\1FLAGS KFS\r\n";message_bytes.push_back('\0');
    const auto write_game=[&](const char* name,std::string_view bytes){std::ofstream output(folder/name,std::ios::binary);output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();check(bool(output)&&chmod((folder/name).c_str(),0644)==0,"local game writes normal readonly-public input");};
    write_game("stored-game.bin",body);write_game("1.MSG",message_bytes);
    client.config.networks.front().msg_netmail_enabled=true;client.config.networks.front().msg_netmail_directory="Doors/Game/NetMail";
    client.config.links.front().port=server.engine->snapshot().classic_port;client.save(62000);client.apply();client.ready();
    await([&]{const auto page=client.db->ftn_queue_page({true,{}},0);return page&&page.value&&std::any_of(page.value->rows.begin(),page.value->rows.end(),[](const auto& row){return row.outbound&&row.file=="stored-game.bin";});},"joined Runtime scanner queues configured Door request without synthetic ACK");
    check(std::filesystem::exists(folder/"1.MSG")&&std::filesystem::exists(folder/"stored-game.bin"),"queued game originals remain before actual peer acknowledgement");
    check(client.engine->poll("TEST","PEER"),"actual native engine accepts game attachment poll");
    await([&]{const auto incoming=server.db->pending_ftn_ingress({true,{}},64);return incoming&&incoming.value&&std::any_of(incoming.value->begin(),incoming.value->end(),[](const auto& item){return item.file.name=="stored-game.bin";});},"actual peer records game payload custody through BinkP");
    await([&]{return !std::filesystem::exists(folder/"1.MSG")&&!std::filesystem::exists(folder/"stored-game.bin");},"real GOT plus joined stored scanner alone completes original KillSent and KFS");
    const auto incoming=server.db->pending_ftn_ingress({true,{}},64);const auto found=std::find_if(incoming.value->begin(),incoming.value->end(),[](const auto& item){return item.file.name=="stored-game.bin";});
    check(found!=incoming.value->end()&&found->network=="TEST"&&found->link=="PEER","received game payload retains configured network and authenticated peer");
    auto file=native::FilePayloadDirectory::private_store(server.root/"FTN/Inbound",server.receipt.bindings).read(found->file.name);const auto actual=file.read(0,body.size());
    check(std::string_view(reinterpret_cast<const char*>(actual.data()),actual.size())==body,"actual peer retains every game attachment byte after sender cleanup");
    sfx::adapters::storage::internal::SqliteConnection read(client.root/"Data/SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
    sfx::adapters::storage::internal::SqliteStatement delivered(read.get(),"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='stored-game.bin' AND state=3","actual game delivery receipt");
    check(delivered.row()&&delivered.int64(0)==1,"one committed actual game delivery, no repeated scanner duplicate");
}
