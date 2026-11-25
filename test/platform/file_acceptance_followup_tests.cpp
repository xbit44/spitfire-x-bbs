// Focused native regression; synthetic scanner, real NTFS/SQLite. No GUI,
// installed authentication or actual malware claim. Old libraries can reproduce
// each case separately without changing any baseline product image/source.
#define main historical_intake_tests_not_run
#include "../integration/file_intake_tests.cpp"
#undef main

int main(int argc,char** argv)try{
    check(argc==3,"fresh bounded root and one regression mode required");
    const auto root=std::filesystem::absolute(argv[1]);const std::string mode=argv[2];
    check(root.parent_path()==ins::Path(L"C:\\sfxtst\\runs")&&root.filename().string().rfind("section9-acceptance-",0)==0&&!std::filesystem::exists(root),"fresh owned acceptance fixture only");
    check(mode=="reservation"||mode=="zip","closed regression mode");
    const auto sid=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(sid);
    for(const auto& p:{root,root/L"Files",root/L"Files"/L"General",root/L"Files"/L"Uploads",root/L"Files"/L"UploadToSysop",root/L"Data",root/L"Data"/L"FileIntake",root/L"Data"/L"FileRemoved"})ins::create_directory(p,descriptor.get());
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};
    storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"real fresh schema");seed(root/L"Data"/L"SFX.DB");check(db.start().succeeded(),"synthetic actors available");
    const app::FileConfigurationActor owner{true,{}};const app::FileActor caller{id(2),2,1,1};
    const auto config=db.file_configuration(owner);check(config&&config.value,"current scan policy");
    app::FileConfigurationChange scan_policy;scan_policy.operation=id(110);scan_policy.expected_catalog_revision=config.value->policy.catalog_revision;scan_policy.policy=config.value->policy;scan_policy.policy.scan_uploads=true;
    check(db.change_file_configuration(owner,scan_policy).succeeded(),"scan On: actually invoke synthetic clean scanner, not Off fallback");
    auto staging=native::FilePayloadDirectory::private_store(root/L"Data"/L"FileIntake",sid);
    auto uploads=native::FilePayloadDirectory::area(root,L"Files/Uploads",{L"Files/General",L"Files/UploadToSysop"},{},sid);
    const auto clean=[](auto&,auto){return app::FileScanResult{app::FileScanVerdict::clean,{},"Synthetic verified clean result"};};
    if(mode=="reservation"){
        const auto fresh=db.file_name_status(owner,{2},{"20meg.zip"});check(fresh&&fresh.value&&!fresh.value->front().registered.value&&!fresh.value->front().pending,"valid first-time name available");
        native::WindowsFileIntake failed(db,owner,caller,{id(100),{2},false,"2026-10-17"},staging);
        const auto held=failed.finish("20meg.zip",false);
        check(held.state==app::FileIntakeState::held&&!failed.record().placed_object,"interrupted empty receive retained privately, never placed");
        const auto status=db.file_name_status(owner,{2},{"20meg.zip"});
        std::cout<<"OBS name pending="<<(status&&status.value&&status.value->size()==1?status.value->front().pending:true)<<std::endl;
        check(status&&status.value&&status.value->size()==1&&!status.value->front().registered.value&&status.value->front().pending&&status.value->front().staging_only_hold,"unplaced failed receive may retry by upload, but still blocks File Manager adoption");
        native::WindowsFileIntake retry(db,owner,caller,{id(101),{2},false,"2026-10-17"},staging);append(retry,"harmless retry");
        check(retry.finish("20meg.zip",true,clean).state==app::FileIntakeState::ready,"same-name fresh retry ready");
        const auto added=retry.accept(root,uploads,{"Retry test",""},false);
        check(added.accepted.has_value()&&added.journal_confirmed,"same-name retry accepted without modifying held payload");
        const auto collision=db.file_name_status(owner,{2},{"20meg.zip"});check(collision&&collision.value->front().registered.value,"accepted final name still protected");
        check(std::filesystem::exists(staging.path()/failed.record().stage_key),"original failed payload preserved, no cleanup or overwrite");
        native::WindowsFileIntake placed(db,owner,caller,{id(103),{2},false,"2026-10-17"},staging,[](auto boundary){if(boundary==native::FileIntakeBoundary::after_placement)throw std::runtime_error("injected placement interruption");});append(placed,"placed bytes");
        check(placed.finish("placed.txt",true,clean).state==app::FileIntakeState::ready,"real placement fixture scan clean");
        check(!placed.accept(root,uploads,{"Interrupted placement",""},false).accepted,"interrupted final placement never claims addition");
        const auto bound=db.file_name_status(owner,{2},{"placed.txt"});check(bound&&bound.value->front().pending&&!bound.value->front().staging_only_hold&&std::filesystem::exists(uploads.path()/L"placed.txt"),"held actual final placement stays reserved and never overwritten");
    }else{
        // A supported scanner's clean verdict must not be overwritten by the
        // optional, deliberately narrower ZIP preview/DIZ parser. No extraction
        // or execution of these harmless opaque bytes occurs in this test.
        native::WindowsFileIntake receive(db,owner,caller,{id(102),{2},false,"2026-10-17"},staging);
        append(receive,"Harmless scanner-clean archive-format regression");
        const auto scanned=receive.finish("dos622.zip",true,clean);
        std::cout<<"OBS state="<<static_cast<unsigned>(scanned.state)<<" scan="<<static_cast<unsigned>(scanned.scan.verdict)<<" detail="<<scanned.detail<<std::endl;
        check(scanned.state==app::FileIntakeState::ready&&scanned.scan.verdict==app::FileScanVerdict::clean,"optional ZIP preview must not substitute a hold for a clean malware verdict");
        const auto added=receive.accept(root,uploads,{"Archive format test",""},false);
        check(added.accepted.has_value()&&added.journal_confirmed,"clean upload immediately added within Uploads");
        const auto stats=db.file_caller_statistics(caller,"2026-10-17");
        check(stats&&stats.value->counts.successful_uploads==1&&stats.value->upload_bytes==receive.record().payload.bytes,"count immediately, no public-area move or approval");
        native::WindowsFileIntake error(db,owner,caller,{id(104),{2},false,"2026-10-17"},staging);append(error,"scan error bytes");
        const auto rejected=error.finish("scanner-error.txt",true,[](auto&,auto){return app::FileScanResult{app::FileScanVerdict::error,{},"Scanner reported an error"};});
        check(rejected.state==app::FileIntakeState::held&&rejected.detail.find("Scanner reported an error")!=std::string::npos&&rejected.detail.find("review")==std::string::npos,"real scan error stays unaccepted with explicit reason, not discretionary inspection");
    }
    check(db.shutdown().succeeded(),"normal storage shutdown");
    std::cout<<"PASS acceptance "<<mode<<" "<<checks<<" checks; real NTFS/SQLite, synthetic scan, no installed/GUI claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL acceptance checks="<<checks<<": "<<e.what()<<'\n';return 1;}
