// Focused real helper/NTFS/SQLite privacy regression. Actors/channels are
// synthetic; this is NOT installed authentication or physical-client evidence.
#define SFX_FILE_HOST_SUPPORT_ONLY
#include "file_transfer_host_tests.cpp"
#include "alpha_file_viewer.hpp"
#include "sfx/platform/win32/file_manager.hpp"

struct PrivateViewerTerminal final:app::ICallerTerminal{
    app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen()override{return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest&)override{return {};}
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{return {};}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest&)override{return {};}
    app::TerminalOutcome present(const app::TerminalPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{return app::TerminalOutcome::completed;}
};

int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"new owned fixture and frozen helper required");
    const auto root=inst::canonical_root(argv[1]);const auto parent=inst::Path(L"C:\\sfxtst\\runs");
    check(root.parent_path()==parent&&root.filename().native().rfind(L"section9-private-collision-r",0)==0&&!std::filesystem::exists(root),"fresh bounded private fixture only");
    auto parent_pin=inst::open_object(parent,true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE);
    const auto owner=inst::operator_sid();const auto sd=inst::mutable_descriptor(owner),structural=inst::structural_descriptor(owner);
    for(const auto& path:{root,root/L"Bin",root/L"Bin"/L"Tools",root/L"Work",root/L"Data",root/L"Data"/L"FileIntake",root/L"Data"/L"FileRemoved",root/L"Files",root/L"Files"/L"General",root/L"Files"/L"Uploads",root/L"Files"/L"UploadToSysop"})inst::create_directory(path,sd.get());
    const auto helper=inst::read_file(argv[2],4*1024*1024);check(inst::sha256(helper)=="81257FC9C96AAAE5BC03FA6EEC7DFAD2BF82A5E9938F0DB52B6ADC2417807C3B","finite-credit general helper exact");
    inst::create_file(root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe",helper,structural.get());protect_file_fixture(root/L"Bin"/L"Tools",structural.get());protect_file_fixture(root/L"Bin",structural.get());protect_file_fixture(root,structural.get());
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock initialized");struct End{~End(){WSACleanup();}} end;
    namespace storage=sfx::adapters::storage;namespace sql=storage::internal;
    sfx::test::FakeStoragePathPolicy policy;storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,policy);check(db.start().succeeded()&&db.shutdown().succeeded(),"real schema initialized");seed_file_actors(root/L"Data"/L"SFX.DB");
    {sql::SqliteConnection seed(root/L"Data"/L"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        seed.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(3,'Bob','bob',10,'active',1,1,1700000000,1700000000);INSERT INTO sfx_account_statistics(account_id) VALUES(3);INSERT INTO sfx_account_profiles(account_id) VALUES(3);INSERT INTO sfx_account_name_reservations VALUES('bob',3,1,'Bob',1700000000,NULL)","second synthetic ordinary caller only");
        sql::SqliteStatement session(seed.get(),"INSERT INTO sfx_account_sessions VALUES(?1,3,3,1700000000,0,1700000000,NULL,NULL,?1)","second synthetic session");session.bind(1,file_operation(3));session.step();}
    check(db.start().succeeded(),"two ordinary actors available");
    unsigned operation=200;const app::FileConfigurationActor host{true,{}};const app::FileActor sysop{file_operation(1),1,1,1};
    // Current behavior: friendly final names, receive before resolving private
    // collisions, no overwrite/existence oracle/success credit for a hold.
    auto current_private=win::FilePayloadDirectory::area(root,"Files/UploadToSysop",{"Files/General","Files/Uploads"},{},owner);
    for(unsigned account=2;account<=3;++account){
        const app::FileActor actor{file_operation(account),static_cast<app::AccountId>(account),1,1};Peer peer;peer.c.limit=GetTickCount64()+12000;
        peer.offer("collision.txt",account==2?"original private":"must not replace",1);peer.finish_receive(2);
        native::CallerFileTransferPorts ports{db,root,owner,[actor]{return std::optional<app::FileActor>{actor};},[&]{return file_operation(operation++);},[]{return std::string("2026-10-17");},peer.ports(),{},[](const auto& row,const auto&,auto){return std::optional<native::CallerUploadReview>{{row.descriptions,row.sysop_only}};}};
        native::AlphaCallerFileTransfers service(std::move(ports));const auto result=service.upload({{"collision.txt",{2},{"Current private",""},false}},true);++cases;
        check(peer.c.acquired&&peer.c.synchronized&&result.connected&&result.files.size()==1,"same-name private submissions both run real helper without pre-transfer existence disclosure");
        check(result.files[0].completed==(account==2)&&result.files[0].held==(account==3),"first friendly-name upload accepted; conflicting upload held without overwrite or false success");
        if(account==2){const auto row=db.managed_file(host,result.files[0].file);check(row&&row.value&&row.value->native_basename.empty()&&app::file_managed_payload_name(*row.value)=="collision.txt","current private catalog addresses the actual original filename");}
        const auto actual=current_private.read("collision.txt").read(0);check(std::string(actual.begin(),actual.end())=="original private","conflicting transfer leaves original bytes unchanged");
    }
    std::vector<native::CallerFileBatchResult> results;std::vector<bool> acquired;
    for(unsigned account=2;account<=3;++account){
        const app::FileActor actor{file_operation(account),static_cast<app::AccountId>(account),1,1};Peer peer;peer.c.limit=GetTickCount64()+12000;
        peer.offer("same.txt",account==2?"first-private-body":"second-private-body",1);peer.finish_receive(2);
        native::CallerFileTransferPorts ports{db,root,owner,[actor]{return std::optional<app::FileActor>{actor};},[&]{return file_operation(operation++);},[]{return std::string("2026-10-17");},peer.ports(),{},[&](const auto& row,const auto&,auto){
            // Fixture ONLY: reconstruct the historical key while still READY,
            // before real prepare/placement/acceptance. Accepted receipts are
            // immutable; do not disable their trigger or rewrite one afterward.
            sql::SqliteConnection seed(root/L"Data"/L"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            sql::SqliteStatement q(seed.get(),"UPDATE sfx_file_intake SET filename_key=':'||stage_key WHERE account_id=?1 AND filename=?2 AND state=1","fixture-only pre-acceptance historical encoding");q.bind(1,actor.account);q.bind(2,row.name);q.step();
            return std::optional<native::CallerUploadReview>{{row.descriptions,row.sysop_only}};}};
        native::AlphaCallerFileTransfers service(std::move(ports));const std::vector<app::FileUploadItem> queue{{"same.txt",{2},{"Private owner submission",""},false}};
        results.push_back(service.upload(queue,true));acquired.push_back(peer.c.acquired);++cases;
        std::cout<<"OBS account="<<account<<" acquired="<<peer.c.acquired<<" connected="<<results.back().connected<<" synchronized="<<peer.c.synchronized;
        for(const auto& row:results.back().files)std::cout<<" completed="<<row.completed<<" held="<<row.held<<" detail="<<row.detail;
        std::cout<<std::endl;
        check(queue[0].destination.value==2&&queue[0].name=="same.txt","private submission leaves normal queue untouched");
        check(results.back().files.size()==1&&results.back().files[0].completed,"historical opaque fixture accepted by the actual unchanged prepare/commit path");
    }
    check(db.shutdown().succeeded(),"DB stops normally even when regression fails");
    check(acquired[0]&&acquired[1],"two ordinary private submissions must both start helper; existing private name must not be an oracle");
    for(const auto& batch:results)check(batch.connected&&batch.files.size()==1&&batch.files[0].completed,"both same friendly filenames accepted without overwrite/disconnect");
    check(results[0].files[0].file.value!=results[1].files[0].file.value,"distinct catalog identities preserve both submissions");
    check(db.start().succeeded(),"reopen for authoritative privacy/counter checks");
    for(unsigned account=2;account<=3;++account){const app::FileActor actor{file_operation(account),static_cast<app::AccountId>(account),1,1};
        for(const auto& batch:results){check(!db.file_information(actor,batch.files[0].file),"ordinary caller cannot inspect either private submission");const auto info=db.file_information(sysop,batch.files[0].file);check(info&&info.value&&info.value->name=="same.txt","Sysop sees original friendly filename");}
        const auto count=db.file_caller_statistics(actor,"2026-10-17");check(count&&count.value&&count.value->counts.successful_uploads==(account==2?2U:1U)&&count.value->counts.eligible_uploads==0,"accepted private uploads count once; conflicting hold grants no credit");}
    const auto config=db.file_configuration(host);check(config&&config.value,"owner configuration available");
    auto private_dir=win::FilePayloadDirectory::area(root,"Files/UploadToSysop",{"Files/General","Files/Uploads"},{},owner);
    const auto first=results[0].files[0].file,second=results[1].files[0].file;
    std::vector<std::string> native_names;
    for(std::size_t n=0;n<results.size();++n){
        const auto managed=db.managed_file(host,results[n].files[0].file);check(managed&&managed.value&&managed.value->native_basename.size()==32,"private native mapping derived from accepted journal");native_names.push_back(managed.value->native_basename);
        const auto payload=private_dir.read(app::file_managed_payload_name(*managed.value));const auto bytes=payload.read(0);const std::string actual(bytes.begin(),bytes.end());const std::string expected=n==0?"first-private-body":"second-private-body";
        check(actual==expected&&payload.identity().sha256==managed.value->payload.sha256,"both original bytes remain distinct and exact");
        Peer peer;peer.c.limit=GetTickCount64()+12000;peer.receive_one("same.txt",static_cast<std::uint32_t>(expected.size()));
        native::CallerFileTransferPorts ports{db,root,owner,[sysop]{return std::optional<app::FileActor>{sysop};},[&]{return file_operation(operation++);},[]{return std::string("2026-10-17");},peer.ports(),{}, {}};
        native::AlphaCallerFileTransfers transfers(std::move(ports));const auto sent=transfers.download({managed.value->file.id});check(sent.connected&&sent.files.size()==1&&sent.files[0].completed&&peer.c.emitted.find("same.txt")!=std::string::npos&&peer.c.emitted.find(managed.value->native_basename)==std::string::npos,"actual helper download keeps friendly wire filename; opaque basename never transmitted");++cases;
        PrivateViewerTerminal terminal;app::FileListPorts list{terminal};list.live=[]{return true;};list.dimensions=[]{return app::MessageEditorDimensions{true,80,24};};list.write=[](std::string_view){return true;};
        native::CallerFileViewerPorts viewer_ports{db,root,owner,[sysop]{return std::optional<app::FileActor>{sysop};},list};viewer_ports.authority=[]{return app::FileAuthority{999,100};};bool displayed{};
        viewer_ports.display=[&](const auto& program,const auto& live){check(live(),"viewer preserves current caller authorization");std::string text;for(const auto& op:program.operations)if(op.kind==app::TerminalDisplayOperationKind::cp437_text)text+=op.bytes;displayed=text.find(expected)!=std::string::npos;return app::TerminalOutcome::completed;};
        native::AlphaFileViewer viewer(std::move(viewer_ports));check(viewer.view(managed.value->file.id,false)&&displayed,"authorized Sysop viewer reads mapped private payload safely");
    }
    check(native_names[0]!=native_names[1]&&private_dir.absent("same.txt"),"same friendly names never share a native destination");
    app::FileQuery query;query.area={3};query.search=app::FileSearchKind::filename;query.query="same.txt";const auto page1=db.file_page(sysop,query,{},1);check(page1&&page1.value&&page1.value->entries.size()==1&&page1.value->next&&page1.value->total_files==2,"Sysop friendly search/summary paginates duplicate private names");
    const auto page2=db.file_page(sysop,query,*page1.value->next,1);check(page2&&page2.value&&page2.value->entries.size()==1&&!(page1.value->entries[0].id==page2.value->entries[0].id),"friendly cursor visits second private submission exactly once");
    app::FileManagementQuery owner_query;owner_query.area={3};owner_query.field=app::FileManagementSearch::filename;owner_query.search="same.txt";const auto found=db.managed_file_page(host,owner_query,{},10);check(found&&found.value&&found.value->files.size()==2,"owner search uses friendly names, not opaque keys");
    win::WindowsFileManager manager(db,host,root,owner);auto scan=manager.find_unregistered({3});check(scan.next().names.empty(),"mapped private blobs are registered, not reimport candidates");
    const auto inspected=manager.inspect_files(owner_query,{},2);check(inspected.entries.size()==2&&std::all_of(inspected.entries.begin(),inspected.entries.end(),[](const auto& e){return e.presence==win::FilePayloadPresence::present;}),"native reconciliation sees both opaque payloads");
    const auto request_for=[&](app::FileId id,app::FileOperationAction action){const auto row=db.managed_file(host,id);check(row&&row.value,"fresh owner metadata for mutation");app::FileOperationRequest r;r.operation=file_operation(operation++);r.action=action;r.file=id;r.expected_revision=row.value->file.revision;return r;};
    // Real absence/reappearance of the SAME owned object; no fabricated native
    // success flag. Restore-missing must retain the existing opaque placement.
    {auto payload=private_dir.read(native_names[0],true);payload.rename_to(private_dir,"temporary-private-away");auto missing=request_for(first,app::FileOperationAction::mark_missing);check(manager.execute(missing).completed,"opaque missing path verified and marked");payload.rename_to(private_dir,native_names[0]);}
    {auto restore=request_for(first,app::FileOperationAction::restore);restore.destination={3};restore.expected_destination_revision=config.value->areas[2].area.revision;restore.filename="same.txt";check(manager.execute(restore).completed,"same opaque native object restored from missing");const auto row=db.managed_file(host,first);check(row&&row.value&&row.value->native_basename==native_names[0],"restore-missing preserves opaque key instead of stranding payload");}
    {auto metadata=request_for(first,app::FileOperationAction::metadata);metadata.descriptions={"Edited private description","Preserve physical mapping"};metadata.public_uploader="Alice";check(manager.execute(metadata).completed,"private metadata edit verifies mapped source");}
    {auto copy=request_for(first,app::FileOperationAction::copy);copy.destination={1};copy.expected_destination_revision=config.value->areas[0].area.revision;copy.filename="private-copy.txt";copy.allow_duplicate_content=true;check(manager.execute(copy).completed,"private payload copies into chosen area under friendly filename");}
    {auto pending=request_for(first,app::FileOperationAction::rename);pending.destination={3};pending.expected_destination_revision=config.value->areas[2].area.revision;pending.filename="cancelled-private.txt";check(db.prepare_file_operation(host,pending).succeeded()&&manager.cancel_unchanged(pending.operation).cancelled,"unchanged opaque source cancellation verifies exact native identity");}
    {auto rename=request_for(first,app::FileOperationAction::rename);rename.destination={3};rename.expected_destination_revision=config.value->areas[2].area.revision;rename.filename="same.txt";
        win::WindowsFileManager interrupted(db,host,root,owner,[](auto at){if(at==win::FileManagerBoundary::after_rename)throw std::runtime_error("synthetic interrupted private rename");});check(!interrupted.execute(rename).completed,"interrupted actual private rename retains unresolved intent");check(manager.reconcile(rename.operation).completed,"private rename reconciles exact placed object");const auto row=db.managed_file(host,first);check(row&&row.value&&row.value->native_basename.empty()&&row.value->file.name==rename.filename&&private_dir.absent(native_names[0]),"renamed payload switches to its new friendly native path");}
    {const auto original=private_dir.read("same.txt").identity();auto clash=request_for(second,app::FileOperationAction::rename);clash.destination={3};clash.expected_destination_revision=config.value->areas[2].area.revision;clash.filename="same.txt";check(!manager.execute(clash).completed,"second legacy normalization refuses existing original-name destination");check(private_dir.read("same.txt").identity().sha256==original.sha256&&!private_dir.absent(native_names[1]),"failed normalization preserves BOTH exact payloads");auto noop=request_for(first,app::FileOperationAction::rename);noop.destination={3};noop.expected_destination_revision=config.value->areas[2].area.revision;noop.filename="same.txt";check(!manager.execute(noop).completed,"ordinary same-path alias remains refused after normalization");}
    {const auto remove=request_for(second,app::FileOperationAction::remove);check(manager.execute(remove).completed,"opaque private payload recoverably removed");auto restore=request_for(second,app::FileOperationAction::restore);restore.destination={3};restore.expected_destination_revision=config.value->areas[2].area.revision;restore.filename="restored-private.txt";check(manager.execute(restore).completed,"removed private payload restores to explicit friendly name");}
    {const auto held_operation=file_operation(operation++);std::string held_name;
        {const app::FileActor alice{file_operation(2),2,1,1};auto staging=win::FilePayloadDirectory::private_store(root/L"Data"/L"FileIntake",owner);win::WindowsFileIntake receive(db,host,alice,{held_operation,{3},true,"2026-10-17"},staging,[](auto at){if(at==win::FileIntakeBoundary::after_placement)throw std::runtime_error("synthetic private placement interruption");});const std::string held="held private bytes";receive.append(reinterpret_cast<const std::uint8_t*>(held.data()),held.size());receive.finish("held.txt",true);check(!receive.accept(root,private_dir,{"Held private",""},false).accepted,"interrupted private placement held, not falsely accepted");held_name=app::file_intake_payload_name(receive.record());check(held_name=="held.txt","even interrupted final placement retains original basename");}
        const auto preview=manager.preview_intake_purge(held_operation);check(preview&&preview->placed,"held opaque placement found by exact native preview");check(manager.purge_held_intake(*preview).completed&&private_dir.absent(held_name),"explicit held purge removes only bound opaque placement");}
    // A pre-opaque private library object is still addressed by its real name.
    {auto legacy=private_dir.create("legacy.txt",64);const std::string body="legacy private bytes";legacy.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());app::FileRegistration r;r.operation=file_operation(operation++);r.area={3};r.expected_area_revision=config.value->areas[2].area.revision;r.filename="legacy.txt";r.descriptions={"Legacy private",""};r.public_uploader="Sysop";r.payload=legacy.identity();const auto registered=db.register_file(host,r);check(registered&&registered.value,"legacy friendly private library fixture registered");const auto row=db.managed_file(host,registered.value->id);check(row&&row.value&&row.value->native_basename.empty()&&app::file_managed_payload_name(*row.value)=="legacy.txt","legacy unmarked rows keep original native path");}
    check(db.shutdown().succeeded(),"final normal DB shutdown");
    std::cout<<"PASS "<<checks<<" checks/"<<cases<<" two-caller private collision cases; native helper/NTFS/SQLite, synthetic actors/channel\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL private collision after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
