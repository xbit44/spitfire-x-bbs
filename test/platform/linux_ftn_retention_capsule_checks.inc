// Linux-specific native behavior, in addition to the unchanged singleton
// policy/order/reference checks. These are real private files and process exits.
{
    const auto capsule_path=[&](const app::FtnRetainedOriginal& value){return processed.path()/(".sfx-expiry-"+native::ftn_native_artifact_name(value.ingress).substr(0,32));};
    original=produce(90);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    intent=prepare(original);removal=probe->seal(intent);const auto held_path=capsule_path(original);
    check(processed.absent(processed_name(original))&&std::filesystem::is_regular_file(held_path/"payload"),"durable intent captures exact original before writer dispatch");
    check(chmod(held_path.c_str(),0500)==0,"actual private directory denies unlink");
    const auto unlink_denied=execute(owner,removal);
    check(!unlink_denied&&removal->result().state==app::FtnExpiryNativeState::disposition_failed&&removal->result().error==EACCES,"real unlink permission failure is not confirmed deletion");
    check(chmod(held_path.c_str(),0700)==0,"restore fixture directory permission");
    check(native::FtnProcessedExpiry::restore_after_dispatch(removal),"explicit settled-dispatch restoration succeeds once permission returns");removal.reset();probe.reset();
    check(processed.read(processed_name(original)).identity().sha256==original.native.sha256&&!std::filesystem::exists(held_path),"failed unlink restores exact original and removes empty capsule");

    original=produce(91);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    intent=prepare(original);removal=probe->seal(intent);const auto occupied_capsule=capsule_path(original);
    app::FilePayloadIdentity occupied_identity;
    {auto replacement=processed.create(processed_name(original),16);replacement.append(reinterpret_cast<const std::uint8_t*>("replacement"),11);occupied_identity=replacement.identity();}
    *cancellation=true;check(!execute(owner,removal)&&removal->result().state==app::FtnExpiryNativeState::not_invoked,"cancel prevents native unlink even with occupied return name");
    removal.reset();probe.reset();*cancellation=false;
    check(processed.read(processed_name(original)).identity().file_id==occupied_identity.file_id,"restore never overwrites newer original-name occupant");
    {auto capsule=native::FilePayloadDirectory::private_store(occupied_capsule,bindings);check(capsule.read("payload").identity().file_id==original.native.file_id,"cancelled original retained in private recovery capsule");}
    bool held{};try{probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);}catch(...){held=true;}
    check(held,"occupied restart recovery remains held for review");
    {auto replacement=processed.read(processed_name(original),true);replacement.rename_to(bad,"occupied-return-name.dat");}
    probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    check(probe->observation()==native::FtnProcessedExpiry::Observation::exact&&!std::filesystem::exists(occupied_capsule),"later recovery restores retained original only when name is vacant");probe.reset();

    original=produce(92);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    intent=prepare(original);probe.reset();check(bool(db.shutdown()),"stop all storage threads before isolated crash child");
    const auto crash_root=root.parent_path()/(root.filename().string()+"-crash");
    check(!std::filesystem::exists(crash_root),"preserve previous crash fixture evidence");
    std::filesystem::copy(root,crash_root,std::filesystem::copy_options::recursive);
    const pid_t child=fork();check(child>=0,"create native crash fixture child");
    if(child==0){
        try{
            auto child_bindings=native::LinuxFilePayloadBindings::prepare(crash_root);native::FtnInstallation child_installation{crash_root,child_bindings};
            auto child_probe=native::FtnProcessedExpiry::inspect(child_installation,original,std::make_shared<std::atomic_bool>(false));
            auto child_removal=child_probe->seal(intent);(void)child_removal;
            _exit(37); // Intent/capture/map durable, no destructor or dispatch.
        }catch(const std::exception& error){std::cerr<<"crash child: "<<error.what()<<std::endl;_exit(38);}
    }
    int child_status{};check(waitpid(child,&child_status,0)==child&&WIFEXITED(child_status)&&WEXITSTATUS(child_status)==37,"actual process exit leaves undispatched recoverable capsule");
    const auto copied=root.parent_path()/(root.filename().string()+"-copied");
    check(!std::filesystem::exists(copied),"preserve previous copied-board evidence");
    std::filesystem::copy(crash_root,copied,std::filesystem::copy_options::recursive);
    auto copied_bindings=native::LinuxFilePayloadBindings::prepare(copied);
    native::FtnInstallation copied_installation{copied,copied_bindings};
    auto copied_probe=native::FtnProcessedExpiry::inspect(copied_installation,original,std::make_shared<std::atomic_bool>(false));
    check(copied_probe->observation()==native::FtnProcessedExpiry::Observation::exact,"whole-folder copy and explicit Prepare recover captured original at new root");copied_probe.reset();
    auto copied_processed=native::FilePayloadDirectory::private_store(copied/"FTN/Processed/TEST",copied_bindings);
    check(copied_processed.read(processed_name(original)).identity().file_id==original.native.file_id,"copied recovery retains portable identity and original bytes");
    auto crash_bindings=native::LinuxFilePayloadBindings::open(crash_root);native::FtnInstallation crash_installation{crash_root,crash_bindings};
    storage::SqliteStorage crash_db(settings(crash_root/"Data"),paths);check(bool(crash_db.start()),"reopen crashed fixture database");
    probe=native::FtnProcessedExpiry::inspect(crash_installation,original,cancellation);
    check(probe->observation()==native::FtnProcessedExpiry::Observation::exact,"same-root restart restores undispatched original");probe.reset();
    pending=crash_db.pending_ftn_expiry(owner,original.ingress);
    check(pending&&pending.value->has_value()&&bool(crash_db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::present_reconciled)),"restart reconciles old intent without replaying deletion");
    check(bool(crash_db.shutdown())&&bool(db.start()),"stop crash fixture and resume original isolated store");

    original=produce(97);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    intent=prepare(original);removal=probe->seal(intent);arm_expiry_fault(capsule_path(original),1);
    *cancellation=true;check(!execute(owner,removal)&&expiry_fault_hits==1,"injected restore fsync failure after reverse rename is reported");
    removal.reset();probe.reset();*cancellation=false;
    check(!processed.absent(processed_name(original)),"reverse-rename failure keeps actual original bytes at return name");
    if(expiry_without_birth){
        bool uncertain{};try{probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);}catch(const std::exception& error){uncertain=std::string_view(error.what())=="linux_file_reconcile_native_identity_unproved";}
        check(uncertain&&std::filesystem::exists(capsule_path(original))&&!processed.absent(processed_name(original)),"no-birth reverse move preserves ambiguous native proof and exact returned bytes");
    }else{
        probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
        check(probe->observation()==native::FtnProcessedExpiry::Observation::exact&&!std::filesystem::exists(capsule_path(original)),"restart reconciles exact reverse recorded move before removing empty capsule");probe.reset();
    }

    original=produce(94);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);intent=prepare(original);
    arm_expiry_fault(processed.path(),1,1);bool capture_flush_failed{};
    try{removal=probe->seal(intent);}catch(...){capture_flush_failed=true;}
    check(capture_flush_failed&&expiry_fault_hits==1,"injected post-capture directory fsync failure is reported");probe.reset();
    check(processed.read(processed_name(original)).identity().sha256==original.native.sha256&&!std::filesystem::exists(capsule_path(original)),"failed capture durability restores original outside writer");
    for(unsigned failure=1;failure<=2;++failure){
        original=produce(94+failure);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
        intent=prepare(original);removal=probe->seal(intent);
        check(bool(db.shutdown()),"stop repository before isolated native uncertainty writer");
        internal::SqlitePagerTelemetry telemetry;internal::SqliteWriterCoordinator writer;
        check(bool(writer.start(database,s,storage::sfx_storage_migrations(),telemetry)),"fresh singleton writer for native uncertainty");
        arm_expiry_fault(failure==1?capsule_path(original):capsule_path(original)/"payload",failure);
        const auto failed=writer.submit_ftn_retention_expiry(removal).get();
        check(!failed&&expiry_fault_hits==1&&removal->result().state==app::FtnExpiryNativeState::close_unconfirmed,"injected post-unlink fsync or released-fd close error stays uncertain");
        check(!writer.shutdown(),"uncertain native completion preserves writer recovery-required status");
        native::FileDescriptor fresh(open("/dev/null",O_RDONLY|O_CLOEXEC));check(bool(fresh),"allocate descriptor after failed native close");
        removal.reset();probe.reset();check(fcntl(fresh.get(),F_GETFD)>=0,"capability teardown does not close a reused descriptor");
        check(bool(db.start()),"reopen after native uncertainty fixture");pending=db.pending_ftn_expiry(owner,original.ingress);
        check(pending&&pending.value->has_value(),"durable original intent survives native completion uncertainty");
        probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
        check(probe->observation()==native::FtnProcessedExpiry::Observation::missing&&bool(db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::absent_uncertain)),"native uncertainty cannot fabricate confirmed removal on restart");probe.reset();
    }

    original=produce(93);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    intent=prepare(original);removal=probe->seal(intent);const auto substituted=capsule_path(original);
    check(rename((substituted/"payload").c_str(),(substituted/"preserved-original").c_str())==0,"fixture substitutes captured name after seal");
    {auto directory=native::open_owned_directory(substituted);native::publish_private_file(directory.get(),"payload",{'n','e','w'},false);}
    check(!execute(owner,removal)&&removal->result().state==app::FtnExpiryNativeState::disposition_failed&&removal->result().error==ESTALE,"writer native capability refuses capture-name substitution");
    removal.reset();probe.reset();
    check(std::filesystem::file_size(substituted/"preserved-original")==original.native.bytes&&std::filesystem::file_size(substituted/"payload")==3,"changed capture preserves both objects for explicit recovery");
}
