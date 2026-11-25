{
    c=*db.ftn_configuration(owner).value;c.networks.front().keep_processed=false;
    for(auto& permission:c.permissions)if(permission.file_echo&&permission.tag=="TEST.FILES")permission.send=false;
    c.permissions.front().receive=true;
    check(bool(db.change_ftn_configuration(owner,id(next++).bytes,c)),"new file originals use Off while old protected originals retain their deadlines");c=*db.ftn_configuration(owner).value;
    auto cancelled=std::make_shared<std::atomic_bool>(false);app::Identifier128 after;
    const auto tick=[&]{const auto page=native::process_ftn_retention_page(db,installed,cancelled,after);check(page.examined<=4,"file-original expiry uses same bounded host page");};
    const auto no_forward=input("retain-empty-plan.zip",'R',false);
    check(publish(no_forward).processed,"new native file publication with durable explicit empty forward plan");
    const auto no_forward_payload=retained_name(no_forward.payload),no_forward_tic=retained_name(no_forward.tic);
    const auto catalog_identity=general.read("retain-empty-plan.zip").identity();
    for(unsigned i=0;i<256&&!processed.absent(no_forward_payload);++i)tick();
    check(processed.absent(no_forward_payload)&&processed.absent(no_forward_tic),"both completed unreferenced native file originals expire");
    check(general.read("retain-empty-plan.zip").identity().file_id==catalog_identity.file_id&&general.read("retain-empty-plan.zip").identity().sha256==catalog_identity.sha256,"published catalog object is untouched by original expiry");
    const auto replaced_file_id=app::FileId{scalar(database,"SELECT file_id FROM sfx_file_catalog WHERE filename='retain-empty-plan.zip'")};
    const auto replacement_original=input("retain-empty-plan.zip",'S',true);
    check(publish(replacement_original).processed,"replacement creates actual retained displacement");
    const auto replacement_original_payload=retained_name(replacement_original.payload),replacement_original_tic=retained_name(replacement_original.tic);
    const auto retained_recovery=db.managed_file(owner,replaced_file_id);check(bool(retained_recovery),"actual displaced catalog record remains queryable");
    const auto recovery_identity=removed.read(retained_recovery.value->removed_key).identity();
    for(unsigned i=0;i<4;++i)tick();
    check(!processed.absent(replacement_original_payload)&&!processed.absent(replacement_original_tic),"outstanding actual displacement preserves both replacement originals");
    check(removed.read(retained_recovery.value->removed_key).identity().file_id==recovery_identity.file_id&&general.read("retain-empty-plan.zip").identity().sha256==tic_hash(replacement_original.bytes),"expiry leaves recovery material and replacement catalog bytes intact");
    auto retention_destination=peer;retention_destination.code="RET";retention_destination.address="2:100/7@test";
    retention_destination.credentials.session=*native::protect_ftn_secret(root.u8string(),"TEST","RET",native::FtnCredentialKind::session,"retention-fixture-session");
    retention_destination.credentials.tic=*native::protect_ftn_secret(root.u8string(),"TEST","RET",native::FtnCredentialKind::tic,"retention-fixture-tic");
    c.links.push_back(retention_destination);c.permissions.push_back({"TEST","RET","TEST.FILES",true,false,true,false});
    check(bool(db.change_ftn_configuration(owner,id(next++).bytes,c)),"one exact new file forwarding destination enabled");c=*db.ftn_configuration(owner).value;
    const auto forwarding_original=input("retain-forward.zip",'T',false);check(publish(forwarding_original).processed,"actual new native publication prepares complete outgoing pair");
    const auto forwarding_original_payload=retained_name(forwarding_original.payload),forwarding_original_tic=retained_name(forwarding_original.tic);
    for(unsigned i=0;i<4;++i)tick();
    check(!processed.absent(forwarding_original_payload)&&!processed.absent(forwarding_original_tic),"unacknowledged actual forwarding plan preserves both source originals");
    auto outgoing_retention=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Outbound"/L"TEST",who);
    std::vector<std::pair<std::string,app::FilePayloadIdentity>> independent_copies;
    const auto retention_generation=id(next++),retention_claim=id(next++);
    for(const auto kind:{app::FtnOutboundKind::payload,app::FtnOutboundKind::tic}){
        const auto jobs=db.claim_ftn_jobs(owner,"TEST","RET",retention_generation,retention_claim,4,false);
        check(jobs&&jobs.value->size()==1&&jobs.value->front().kind==kind,"real queue exposes exact payload then paired TIC");
        const auto& job=jobs.value->front();const auto name=native::ftn_existing_spool_name(outgoing_retention,job);
        independent_copies.emplace_back(name,outgoing_retention.read(name).identity());
        check(bool(db.finish_ftn_job(owner,job.id,retention_claim,app::FtnOutboundState::delivered,{})),"synthetic host GOT port records exact forwarding acknowledgement");
    }
    for(unsigned i=0;i<256&&!processed.absent(forwarding_original_payload);++i)tick();
    check(processed.absent(forwarding_original_payload)&&processed.absent(forwarding_original_tic),"completed acknowledged independent forwarding releases original retention dependency");
    for(const auto& copy:independent_copies){const auto current=outgoing_retention.read(copy.first).identity();check(current.file_id==copy.second.file_id&&current.sha256==copy.second.sha256,"original expiry does not remove or alter independent outgoing spool object");}
    check(general.read("retain-forward.zip").identity().sha256==tic_hash(forwarding_original.bytes),"forwarded catalog bytes remain intact");
}
