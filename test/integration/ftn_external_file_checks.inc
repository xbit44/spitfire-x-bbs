int external_file_fixture(Fixture& board){
    const auto& root=board.root;
#ifdef _WIN32
    const auto descriptor=ins::mutable_descriptor(board.receipt.owner);
    ins::create_directory(root/"Config",descriptor.get());check(bool(native::create_portable_board_key(root,board.receipt.owner)),"file fixture portable key");
    for(const auto* path:{"Data/FileRemoved","Data/FileIntake","Files","Files/General","Files/Uploads","Files/UploadToSysop"})ins::create_directory((root/path).make_preferred(),descriptor.get());
    const auto& bindings=board.receipt.owner;
#else
    for(const auto* path:{"Data/FileRemoved","Data/FileIntake","Files","Files/General","Files/Uploads","Files/UploadToSysop"})make_directory(root/path);
    const auto& bindings=board.receipt.bindings;
#endif
    board.config.links.front().credentials={};board.config.links.front().poll_minutes=0;
    app::FtnNetwork second;second.code="OTHER";second.name="Shared second file network";second.address="3:200/1@other";second.enabled=true;board.config.networks.push_back(second);
    auto link=board.config.links.front();link.network="OTHER";link.address="3:200/2@other";board.config.links.push_back(link);
    for(auto& peer:board.config.links){
        const auto secret=native::protect_ftn_secret(root.u8string(),peer.network,peer.code,native::FtnCredentialKind::tic,"fixture-tic-only");check(bool(secret),"separate configured TIC secret");peer.credentials.tic=*secret;
        board.config.file_echoes.push_back({peer.network,"SHARED.FILES","Shared file echo",1});board.config.permissions.push_back({peer.network,peer.code,"SHARED.FILES",true,true,true,false});
    }
    board.save(2);external_start(board);
    const std::string first_bytes="Exact TEST network payload through shared stock BinkD.";
    const std::string second_bytes="Exact OTHER network payload through shared stock BinkD.";
    const auto fixture_tic=[&](std::string_view network,std::string filename,std::string_view bytes,std::string password){
        const auto peer=std::find_if(board.config.links.begin(),board.config.links.end(),[&](const auto& p){return p.network==network;});const auto own=std::find_if(board.config.networks.begin(),board.config.networks.end(),[&](const auto& n){return n.code==network;});
        app::FtnTic tic;tic.area="SHARED.FILES";tic.file=std::move(filename);tic.origin=tic.from=*app::parse_ftn_address(peer->address);tic.to=*app::parse_ftn_address(own->address);tic.size=bytes.size();tic.crc=app::ftn_crc32(bytes);tic.password=std::move(password);tic.descriptions={"Real shared stock file qualification"};
        tic.path.push_back({tic.from,std::uint64_t{1700000000},{}});
        const auto encoded=app::encode_ftn_tic(tic);check(bool(encoded),"stock file TIC encoded");return *encoded;
    };
    external_fixture_write(root/"test-data.dat",first_bytes);external_fixture_write(root/"other-data.dat",second_bytes);
    external_fixture_write(root/"test-control.tic",fixture_tic("TEST","test-data.dat",first_bytes,"fixture-tic-only"));
    external_fixture_write(root/"other-control.tic",fixture_tic("OTHER","other-data.dat",second_bytes,"fixture-tic-only"));
    external_fixture_write(root/"bad-data.dat","Rejected separate TIC password.");external_fixture_write(root/"bad-control.tic",fixture_tic("TEST","bad-data.dat","Rejected separate TIC password.","wrong-fixture-tic"));
    // LOCAL hatch must preserve its source and use the same pair publisher.
    const std::string local_bytes="Local hatch source retained after external stock transfer.";app::FilePayloadIdentity local_identity;
    {auto directory=native::FilePayloadDirectory::area(root,L"Files/General",{L"Files/Uploads",L"Files/UploadToSysop"},{L"FTN"},bindings);auto file=directory.create("local-hatch.dat",local_bytes.size());file.append(reinterpret_cast<const std::uint8_t*>(local_bytes.data()),local_bytes.size());local_identity=file.identity();}
    app::FileRegistration registration;registration.operation=id(940);registration.area={1};registration.expected_area_revision=1;registration.filename="local-hatch.dat";registration.descriptions={"Local hatch fixture","Preserved local description"};registration.public_uploader="Sysop";registration.payload=local_identity;registration.original_upload_utc=1700000000;
    const auto registered=board.db->register_file({true,{}},registration);check(registered&&registered.value,"LOCAL source registered");
    app::FtnHatchRequest hatch;hatch.operation=id(941);hatch.file=registered.value->id;hatch.file_revision=registered.value->revision;hatch.configuration_revision=board.config.revision;hatch.network="TEST";hatch.tag="SHARED.FILES";hatch.origin=board.config.networks.front().address;hatch.description="Reviewed local hatch";hatch.destinations={"PEER"};
    const auto queued=native::hatch_ftn_native_file(*board.db,*board.db,board.receipt,hatch,[]{return true;});check(queued.queued&&queued.journaled,"real LOCAL hatch queued payload/TIC pair");
    external_fixture_write(root/"READY","shared file fixture ready\n");bool phase1{};const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(150);
    while(std::chrono::steady_clock::now()<deadline){
        if(!phase1&&std::filesystem::exists(root/"CHECK_PHASE1")){
            check(!std::filesystem::exists(root/"Files/General/test-data.dat")&&!std::filesystem::exists(root/"Files/General/other-data.dat"),"neither TIC-only nor payload-only arrival publishes a file");phase1=true;external_fixture_write(root/"PHASE1","both incomplete pairs retained\n");
        }
        if(phase1&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4")==4&&std::filesystem::exists(root/"Files/General/test-data.dat")&&std::filesystem::exists(root/"Files/General/other-data.dat")&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_tic_rejections WHERE reason='ftn_tic_password_wrong'")==1)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    check(phase1,"arrival-order phases completed");board.engine->stop();
    for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"test-data.dat",first_bytes},{"other-data.dat",second_bytes},{"local-hatch.dat",local_bytes}}){auto directory=native::FilePayloadDirectory::area(root,L"Files/General",{L"Files/Uploads",L"Files/UploadToSysop"},{L"FTN"},bindings);auto file=directory.read(item.first);const auto bytes=file.read(0,item.second.size());check(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size())==item.second,"published or LOCAL source bytes exact");}
    check(!std::filesystem::exists(root/"Files/General/bad-data.dat"),"P=1 cannot bypass configured wrong TIC password");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4")==4,"both valid TIC/payload pairs completed");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_tic_rejections WHERE reason='ftn_tic_password_wrong'")==1,"wrong separate TIC password durably rejected without changing transport custody");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")==2&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==0,"LOCAL hatch pair handed off without false Delivered");
    app::FileQuery query;query.area={1};const auto catalog=board.db->file_page({id(2),2,1,1},query,{},20);check(catalog&&catalog.value->entries.size()==3,"two FTN files and retained LOCAL source in actual catalog");
    check(board.db->managed_file({true,{}},hatch.file).value->file.descriptions.long_text=="Preserved local description","LOCAL hatch preserves source description");
    check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"file fixture references clean");check(board.db->shutdown().succeeded(),"file fixture store closed");external_fixture_write(root/"DONE","PASS shared TIC/payload and LOCAL hatch\n");std::cout<<"PASS shared file Runtime host "<<checks<<" checks\n";return 0;
}
