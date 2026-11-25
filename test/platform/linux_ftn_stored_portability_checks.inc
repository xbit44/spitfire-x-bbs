void stored_portability_checks(const std::filesystem::path& root){
    check(!std::filesystem::exists(root)&&mkdir(root.c_str(),0700)==0,"new portable Door source board");
    for(const auto* role:{"Config","Data","FTN","FTN/Outbound","FTN/Outbound/TEST","FTN/Outbound/OTHER","Doors","Doors/Game","Doors/Game/NetMail","Doors/Game/netmail"})
        check(mkdir((root/role).c_str(),0700)==0,"private portable source role");
    {auto directory=native::open_owned_directory(root/"Config");const std::string first="SFX_LINUX_FIRST_RUN=1\n";native::publish_private_file(directory.get(),"first-run.v1",{first.begin(),first.end()},false);}
    check(bool(native::initialize_fresh_portable_board_key(root)),"portable Door source board key");
    auto bindings=native::LinuxFilePayloadBindings::initialize_fresh(root);native::FtnInstallation installed{root,bindings};
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings(root/"Data"),paths);
    check(bool(db.start())&&bool(db.shutdown()),"portable source schema");const auto database=root/"Data/SFX.DB";seed(database);check(bool(db.start()),"portable source store");
    const app::MessageConfigurationActor owner{true,{}};auto config=*db.ftn_configuration(owner).value;
    for(const auto* code:{"TEST","OTHER"}){
        const bool upper=std::string_view(code)=="TEST";app::FtnNetwork network;network.code=code;network.name=code;network.address="2:100/2@test";network.enabled=true;network.msg_netmail_enabled=true;
        network.msg_netmail_directory=upper?"Doors/Game/NetMail":"Doors/Game/netmail";config.networks.push_back(network);
        app::FtnLink peer;peer.network=code;peer.code="PEER";peer.name="Isolated source peer";peer.address="2:100/1@test";peer.host="127.0.0.1";peer.enabled=true;config.links.push_back(peer);
        const auto folder=root/network.msg_netmail_directory;local_game_file(folder/"attach.bin",upper?"uppercase source payload":"lowercase source payload");
        for(unsigned i=1;i<=9;++i)local_game_file(folder/(std::to_string(i)+".MSG"),stored_message("attach.bin",0x111,""));
    }
    check(bool(db.change_ftn_configuration(owner,id(900).bytes,config)),"save two case-distinct relative Door source networks");config=*db.ftn_configuration(owner).value;
    std::string cursor;const auto scan=[&]{return native::scan_ftn_stored_messages(db,config,installed,cursor,[]{return true;});};
    auto result=scan();if(result.failed)std::cerr<<result.reason<<'\n';
    check(!result.failed&&result.queued==8,"first bounded batch scans eight requests in first case-distinct directory");
    const auto first_cursor=cursor;check(!cursor.empty()&&cursor.back()!=':',"first directory has bounded continuation cursor");
    result=scan();check(!result.failed&&result.queued==1&&cursor.back()==':',"first source completion advances cursor to second source");
    const auto second_cursor=cursor;check(first_cursor.substr(0,first_cursor.find(':'))!=second_cursor.substr(0,second_cursor.find(':')),"case-distinct sources have different cursor identities");
    result=scan();check(!result.failed&&result.queued==8,"second network gets its own bounded batch");
    result=scan();check(!result.failed&&result.queued==1&&cursor.substr(0,cursor.find(':'))==first_cursor.substr(0,first_cursor.find(':')),"cursor fairly cycles back after completing second network");
    for(const auto* code:{"TEST","OTHER"}){
        const auto jobs=db.claim_ftn_jobs(owner,code,"PEER",id(901),id(std::string_view(code)=="TEST"?902:903),64,false);
        check(jobs&&jobs.value->size()==9&&std::all_of(jobs.value->begin(),jobs.value->end(),[&](const auto& job){return job.network==code&&job.link=="PEER";}),"each source queues only its configured network scope");
        // Leave the durable requests pending; no synthetic delivery is needed
        // to prove identity survives a stopped whole-folder copy.
    }
    const auto original_count=scalar(database,"SELECT count(*) FROM sfx_ftn_outbound");
    bindings->refresh();check(bool(db.checkpoint())&&bool(db.shutdown()),"stop portable Door board before whole-folder copy");
    const auto copied=root.parent_path()/(root.filename().string()+"-copied");check(!std::filesystem::exists(copied),"new copied Door board");std::filesystem::copy(root,copied,std::filesystem::copy_options::recursive);
    auto rebound=native::LinuxFilePayloadBindings::prepare(copied);check(rebound->deferred_records()==0,"intact copied Door request and attachment bindings rebind normally");
    native::FtnInstallation copy{copied,rebound};storage::SqliteStorage copy_db(settings(copied/"Data"),paths);check(bool(copy_db.start()),"copied Door board opens");const auto copy_config=*copy_db.ftn_configuration(owner).value;
    check(copy_config.networks.front().msg_netmail_directory=="Doors/Game/NetMail","relative Door configuration is preserved without path conversion");
    cursor.clear();for(unsigned i=0;i<4;++i){const auto copied_scan=native::scan_ftn_stored_messages(copy_db,copy_config,copy,cursor,[]{return true;});if(copied_scan.failed)std::cerr<<copied_scan.reason<<'\n';check(!copied_scan.failed&&!copied_scan.queued&&!copied_scan.completed,"copied intact pending Door requests retain original operations without duplicate or hold");}
    check(scalar(copied/"Data/SFX.DB","SELECT count(*) FROM sfx_ftn_outbound")==original_count&&original_count==18,"both case-distinct copied networks retain exact eighteen pending jobs");
    for(const auto* directory:{"NetMail","netmail"}){
        const auto before=native::FilePayloadDirectory::import_source(root/"Doors/Game"/directory,{},bindings,true);
        const auto after=native::FilePayloadDirectory::import_source(copied/"Doors/Game"/directory,{},rebound,true);
        for(const auto* name:{"1.MSG","attach.bin"}){auto left=before.read(name);auto right=after.read(name);const auto a=left.identity(),b=right.identity();check(a.volume==b.volume&&a.file_id==b.file_id&&a.sha256==b.sha256,"copied Door request and attachment retain logical identity and exact bytes");}
    }
    check(bool(copy_db.shutdown()),"copied Door board stops");
}
