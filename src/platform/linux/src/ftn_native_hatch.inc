FtnNativeHatchResult hatch_ftn_native_file(app::IFtnMailStorage& storage,app::IFileStorage& files,
    const FtnInstallation& installation,const app::FtnHatchRequest& request,
    const std::function<bool()>& running,const std::function<bool(std::uint64_t,std::uint64_t)>& progress,
    const std::function<void(std::string_view)>& boundary){
    FtnNativeHatchResult result;
    try{
        require(app::valid_ftn_hatch_request(request),"ftn_hatch_request_invalid");verify_ftn_directory(installation);
        const auto previous=storage.ftn_hatch(owner,request.operation);
        if(previous&&previous.value){require(app::encode_ftn_hatch_request(previous.value->request)==app::encode_ftn_hatch_request(request),"ftn_hatch_changed_request");result.journaled=true;if(previous.value->state==app::FtnHatchState::queued){result.queued=true;return result;}}
        else require(previous.status.code==app::StorageErrorCode::not_found,"ftn_hatch_lookup_failed");
        const auto saved=storage.ftn_configuration(owner);require(saved&&saved.value,"ftn_hatch_configuration_unavailable");const auto configuration=*saved.value;
        const auto selected=files.managed_file(owner,request.file);require(selected&&selected.value,"ftn_hatch_source_unavailable");const auto original=*selected.value;
        const auto current=[&]{
            require(running&&running(),"ftn_hatch_cancelled");const auto c=storage.ftn_configuration(owner);const auto serial=storage.ftn_serial_state(owner);
            require(c&&c.value&&c.value->revision==request.configuration_revision&&c.value->settings.mode!=app::FtnMailerMode::disabled&&serial&&serial.value&&!serial.value->recovery_required,"ftn_hatch_configuration_changed");
            for(const auto& destination:request.destinations)require(app::ftn_echo_permission(*c.value,request.network,destination,request.tag,true,false),"ftn_hatch_send_permission_changed");
            const auto source=files.managed_file(owner,request.file);require(source&&source.value&&!source.value->purged&&!source.value->operation_pending&&source.value->file.state==app::FileCatalogState::available&&source.value->file.revision==request.file_revision&&source.value->file.name==original.file.name&&source.value->file.area==original.file.area&&tic_same_payload(source.value->payload,original.payload),"ftn_hatch_source_changed");
        };
        const auto point=[&](std::string_view name){current();if(boundary)boundary(name);};current();
        const auto network=std::find_if(configuration.networks.begin(),configuration.networks.end(),[&](const auto& value){return same(value.code,request.network);});const auto origin=app::parse_ftn_address(request.origin);
        require(network!=configuration.networks.end()&&origin&&app::ftn_local_address(*network,*origin),"ftn_hatch_origin_not_owned");
        const auto catalog=files.file_configuration(owner);require(catalog&&catalog.value,"ftn_hatch_file_configuration_unavailable");const auto area=std::find_if(catalog.value->areas.begin(),catalog.value->areas.end(),[&](const auto& a){return a.area.id==original.file.area;});require(area!=catalog.value->areas.end(),"ftn_hatch_source_area_unavailable");
        std::vector<std::filesystem::path> others;for(const auto& a:catalog.value->areas)if(!(a.area.id==original.file.area))others.push_back(std::filesystem::u8path(a.storage_path));
        const auto directory=FilePayloadDirectory::area(installation.root,std::filesystem::u8path(area->storage_path),others,{L"bin",L"config",L"data",L"display",L"backup",L"logs",L"work",L"doors",L"botgate",L"ftn"},installation.bindings);
        auto source=directory.read(app::file_managed_payload_name(original));const auto tick=[&](std::uint64_t done,std::uint64_t total){current();require(!progress||progress(done,total),"ftn_hatch_cancelled");return true;};
        const auto identity=source.identity(tick);require(identity.bytes<=app::kFtnTransferMaximumBytes&&tic_same_payload(identity,original.payload),"ftn_hatch_source_bytes_changed");app::FtnCrc32 checksum;
        for(std::uint64_t at=0;at<identity.bytes;){tick(at,identity.bytes);const auto bytes=source.read(at,static_cast<std::size_t>(std::min<std::uint64_t>(65536,identity.bytes-at)));require(!bytes.empty(),"ftn_hatch_source_read_failed");checksum.update({reinterpret_cast<const char*>(bytes.data()),bytes.size()});at+=bytes.size();}
        const auto prepared=storage.prepare_ftn_hatch(owner,request,identity,checksum.value());require(prepared&&prepared.value,"ftn_hatch_prepare_failed");result.journaled=true;const auto record=*prepared.value;point("hatch_prepared");
        auto root=FilePayloadDirectory::private_store(installation.root/L"ftn",installation.bindings);
        const auto child=[&](const std::filesystem::path& path){current();return create_ftn_directory(installation,path);};
        const auto parent=child(root.path()/L"outbound"),outgoing=child(parent.path()/std::filesystem::u8path(layout::owned_name(network->code)));
        // SHARE_READ plus DELETE access excludes another mutable preparer lease
        // across processes. The retained object is bound durably; an unbound
        // create gap is held, never adopted, replaced or cleaned up.
        const auto lock_name=ftn_native_artifact_name(request.operation)+".hatch-lock";std::optional<FilePayloadLease> preparer;
        if(record.preparer){preparer.emplace(outgoing.read(lock_name,true));require(tic_same_object(preparer->object_identity(),*record.preparer)&&preparer->byte_count()==0,"ftn_hatch_preparer_changed");}
        else{require(outgoing.absent(lock_name),"ftn_hatch_unbound_preparer");preparer.emplace(outgoing.create(lock_name,1));point("hatch_preparer_created_before_binding");const auto bound=storage.bind_ftn_hatch_preparer(owner,request.operation,preparer->object_identity());require(bound&&bound.value&&bound.value->preparer,"ftn_hatch_preparer_binding_failed");}
        point("hatch_preparer_bound");
        std::vector<const app::FtnLink*> destinations;std::vector<app::FtnAddress> seen_by{*origin};
        for(const auto& code:request.destinations){const auto link=std::find_if(configuration.links.begin(),configuration.links.end(),[&](const auto& l){return same(l.network,request.network)&&same(l.code,code);});require(link!=configuration.links.end(),"ftn_hatch_destination_unavailable");const auto address=app::parse_ftn_address(link->address);require(address&&!app::ftn_local_address(*network,*address),"ftn_hatch_destination_invalid");destinations.push_back(&*link);if(std::none_of(seen_by.begin(),seen_by.end(),[&](const auto& a){return app::same_ftn_address(a,*address,true);}))seen_by.push_back(*address);}
        std::vector<app::FtnOutboundJob> jobs;std::vector<FilePayloadLease> leases;
        for(const auto* link:destinations){
            current();app::FtnOutboundJob payload;payload.operation=request.operation;payload.id=operation(request.operation,0,link->code+"/local-hatch-payload");payload.ordinal=static_cast<std::uint16_t>(jobs.size());payload.network=network->code;payload.link=link->code;payload.kind=app::FtnOutboundKind::payload;payload.file={record.filename,identity.bytes,static_cast<std::uint64_t>(record.created)};payload.digest=identity.sha256;
            app::FtnTic metadata;TextErase wipe_password{metadata.password};metadata.area=request.tag;metadata.file=record.filename;metadata.origin=metadata.from=*origin;metadata.to=*app::parse_ftn_address(link->address);metadata.size=identity.bytes;metadata.crc=record.crc;metadata.path.push_back({*origin,static_cast<std::uint64_t>(record.created),{}});metadata.seen_by=seen_by;
            std::istringstream lines(request.description);std::string line;while(std::getline(lines,line))if(!line.empty())metadata.long_descriptions.push_back(line);metadata.descriptions.push_back(app::sanitize_file_text(metadata.long_descriptions.empty()?record.filename:metadata.long_descriptions.front(),app::kFileShortDescriptionBytes,false));
            if(!link->credentials.tic.empty()){auto secret=unprotect_ftn_secret(installation.root.u8string(),network->code,link->code,FtnCredentialKind::tic,link->credentials.tic);require(secret.has_value(),"ftn_hatch_secret_unavailable");metadata.password.assign(secret->view());}
            auto encoded=app::encode_ftn_tic(metadata);require(encoded.has_value(),"ftn_hatch_metadata_invalid");Bytes bytes{std::move(*encoded)};app::FtnOutboundJob tic=payload;tic.id=operation(request.operation,0,link->code+"/local-hatch-tic");tic.ordinal=static_cast<std::uint16_t>(payload.ordinal+1);tic.kind=app::FtnOutboundKind::tic;tic.file.name=ftn_native_artifact_name(tic.id).substr(0,32)+".tic";tic.file.size=bytes.value.size();tic.digest=digest(bytes.value);const std::string_view wire=bytes.value;
            leases.push_back(materialize_ftn_spool(storage,request.configuration_revision,outgoing,payload,source,nullptr,current,point,progress));jobs.push_back(payload);leases.push_back(materialize_ftn_spool(storage,request.configuration_revision,outgoing,tic,source,&wire,current,point,progress));jobs.push_back(tic);
        }
        point("hatch_before_queue");const auto queued=storage.commit_ftn_hatch(owner,request.operation,jobs);require(queued&&queued.value&&queued.value->state==app::FtnHatchState::queued,"ftn_hatch_queue_commit_failed");result.queued=true;if(boundary)boundary("hatch_after_queue");
    }catch(const std::exception& error){result.reason=reason(error);if(result.journaled){const auto durable=storage.ftn_hatch(owner,request.operation);if(durable&&durable.value&&durable.value->state==app::FtnHatchState::queued){result.queued=true;result.reason.clear();}else (void)storage.hold_ftn_hatch(owner,request.operation,result.reason);}}
    return result;
}
