application::FtnHeldControlResponse LinuxRuntimeHost::ftn_held(const application::FtnHeldControlRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_ready())return {};
    return application::execute_ftn_held_control(*store_.storage(),{true,{}},request);
}
application::FtnRuntimeResponse LinuxRuntimeHost::ftn_runtime_checked(const application::FtnRuntimeRequest& request,application::Identifier128 epoch,std::int64_t revision){
    namespace app=application;std::lock_guard<std::recursive_mutex> lock(operations_);
    auto current=ftn_runtime({});
    if(app::encode_ftn_runtime_request(request).empty()||epoch!=epoch_||revision<=0||current.code!=app::FtnConfigurationCode::ok||current.saved_revision!=revision||current.active_revision!=revision||
        !current.capable||current.paused||current.serial_recovery_required||current.mode==app::FtnMailerMode::disabled||(request.action==app::FtnRuntimeAction::poll&&current.mode!=app::FtnMailerMode::native)||
        request.include_hold||(request.action!=app::FtnRuntimeAction::poll&&request.action!=app::FtnRuntimeAction::scan)){
        current.code=app::FtnConfigurationCode::conflict;return current;
    }
    // AreaFix can activate configuration on the FTN worker independently of
    // the Host operation lock. Recheck the reviewed revision under its lock.
    const bool accepted=ftn_engine_&&(request.action==app::FtnRuntimeAction::poll?
        ftn_engine_->poll(request.network,request.link,false,revision):ftn_engine_->scan(revision));
    current=ftn_runtime({});if(!accepted)current.code=app::FtnConfigurationCode::conflict;return current;
}
application::FtnRuntimeResponse LinuxRuntimeHost::ftn_runtime(const application::FtnRuntimeRequest& request){
    namespace app=application;std::lock_guard<std::recursive_mutex> lock(operations_);app::FtnRuntimeResponse out;
    if(app::encode_ftn_runtime_request(request).empty()){out.code=app::FtnConfigurationCode::invalid;return out;}
    if(!store_ready())return out;
    out.code=app::FtnConfigurationCode::ok;
    if(request.action==app::FtnRuntimeAction::poll&&(!ftn_engine_||!ftn_engine_->poll(request.network,request.link,request.include_hold)))out.code=app::FtnConfigurationCode::unavailable;
    if(request.action==app::FtnRuntimeAction::scan&&(!ftn_engine_||!ftn_engine_->scan()))out.code=app::FtnConfigurationCode::unavailable;
    if(ftn_engine_){const auto state=ftn_engine_->snapshot();out.mode=state.mode;out.saved_revision=state.saved_revision;out.active_revision=state.active_revision;out.capable=state.capable;out.paused=state.paused;out.serial_recovery_required=state.serial_recovery_required;out.classic_listening=state.classic_listening;out.tls_listening=state.tls_listening;out.processing=state.processing;out.classic_port=state.classic_port;out.tls_port=state.tls_port;out.diagnostic=state.diagnostic;out.classic_error=state.classic_error;out.tls_error=state.tls_error;out.processing_error=state.processing_error;out.processed_receipts=state.processed_receipts;out.held_receipts=state.held_receipts;out.imported_messages=state.imported_messages;out.replayed_messages=state.replayed_messages;out.scanned_messages=state.scanned_messages;out.activity=state.activity;
        for(const auto& s:state.sessions){const auto& t=s.transfer;out.sessions.push_back({s.serial,s.active,t.outbound,t.completed,t.peer_verified,t.network,t.link,t.reason,t.received_bytes,t.sent_bytes,t.received_files,t.sent_files,t.send,t.receive,{},{}});}
    }else out.diagnostic=ftn_initialization_error_;
    if(request.action==app::FtnRuntimeAction::queues){const auto page=store_.storage()->ftn_queue_page({true,{}},request.offset);if(page&&page.value)out.queues=*page.value;else out.code=app::FtnConfigurationCode::unavailable;}
    if(request.action==app::FtnRuntimeAction::connections){const auto c=store_.storage()->ftn_configuration({true,{}});if(!c||!c.value)out.code=app::FtnConfigurationCode::unavailable;else{const auto& links=c.value->links;const auto end=std::min<std::size_t>(links.size(),static_cast<std::size_t>(request.offset)+64);for(auto i=static_cast<std::size_t>(request.offset);i<end;++i){const auto& link=links[i];const auto n=std::find_if(c.value->networks.begin(),c.value->networks.end(),[&](const auto& network){return network.code==link.network;});out.connections.push_back({link.network,link.code,(n!=c.value->networks.end()?n->name:std::string("Unavailable network"))+" / "+link.name,link.enabled&&n!=c.value->networks.end()&&n->enabled,app::ftn_outbound(link)});}out.more_connections=end<links.size();}}
    // These are observation copies, never storage/routing objects. Keep private
    // routing keys only in connection choices used by Poll requests.
    const auto catalog=store_.storage()->ftn_configuration({true,{}});
    if(!ftn_engine_&&catalog&&catalog.value){out.saved_revision=catalog.value->revision;out.mode=catalog.value->settings.mode;}
    const auto names=[&](std::string& network,std::string& link){
        const auto network_code=network,link_code=link;network="Unavailable network";link="Unconfigured peer";
        if(catalog&&catalog.value){for(const auto& n:catalog.value->networks)if(n.code==network_code){network=n.name;break;}
            for(const auto& l:catalog.value->links)if(l.network==network_code&&l.code==link_code){link=l.name;break;}}
    };
    for(auto& s:out.sessions){
        if(catalog&&catalog.value)for(const auto& l:catalog.value->links)if(l.network==s.network&&l.code==s.link){s.peer_address=l.address;s.endpoint=l.host.empty()?std::string{}:l.host+":"+std::to_string(l.port);break;}
        names(s.network,s.link);
    }
    for(auto& a:out.activity)names(a.network,a.link);
    if(out.queues)for(auto& row:out.queues->rows)names(row.network,row.link);
    return out;
}
application::FtnConfigurationResponse LinuxRuntimeHost::ftn_hatch_catalog(const application::FtnConfigurationRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    if(!ftn_hatch_publisher_)ftn_hatch_publisher_=std::make_unique<application::FtnReadOnlyPublisher>([this]()->std::optional<application::FtnReadOnlyPublisher::Snapshot>{const auto c=store_.storage()->ftn_configuration({true,{}});if(!c||!c.value)return {};auto bytes=application::encode_ftn_hatch_catalog(application::ftn_hatch_catalog(*c.value));if(bytes.empty())return {};return application::FtnReadOnlyPublisher::Snapshot{c.value->revision,std::move(bytes)};});
    return ftn_hatch_publisher_->handle(request,clock_.monotonic_millis());
}
application::FtnConfigurationResponse LinuxRuntimeHost::ftn_configuration(const application::FtnConfigurationRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_ready())return {};
    if(!ftn_publisher_)ftn_publisher_=std::make_unique<application::FtnConfigurationPublisher>(
        [this]()->std::optional<application::FtnConfiguration>{
            const auto c=store_.storage()->ftn_configuration({true,{}});return c&&c.value?c.value:std::nullopt;
        },[this](const application::FtnTransferToken& token,const application::FtnConfiguration& c){
            application::FtnConfigurationResponse response;
            const auto current=store_.storage()->ftn_configuration({true,{}});
            if(!current||!current.value||!platform::linux_host::valid_changed_ftn_credentials(root_.u8string(),*current.value,c)){response.code=application::FtnConfigurationCode::invalid;return response;}
            const auto changed=store_.storage()->change_ftn_configuration({true,{}},token,c);
            if(changed&&changed.value){if(ftn_engine_)ftn_engine_->configure(changed.value->configuration);response.code=application::FtnConfigurationCode::ok;response.revision=changed.value->applied_revision;}
            else response.code=changed.status.code==application::StorageErrorCode::backpressure?application::FtnConfigurationCode::busy:changed.status.code==application::StorageErrorCode::conflict?application::FtnConfigurationCode::conflict:application::FtnConfigurationCode::unavailable;
            return response;
        },[this](const application::FtnTransferToken& token){
            application::FtnConfigurationResponse response;const auto receipt=store_.storage()->ftn_configuration_receipt({true,{}},token);
            if(receipt&&receipt.value){response.revision=*receipt.value;response.code=response.revision?application::FtnConfigurationCode::ok:application::FtnConfigurationCode::conflict;}
            return response;
        },[this](const application::FtnTransferToken& token,const application::FtnConfiguration& c){
            application::FtnConfigurationResponse response;
            const auto current=store_.storage()->ftn_configuration({true,{}});
            if(!current||!current.value||!platform::linux_host::valid_changed_ftn_credentials(root_.u8string(),*current.value,c)){response.code=application::FtnConfigurationCode::invalid;return response;}
            const auto review=store_.storage()->preview_ftn_retention({true,{}},token,c);
            if(review&&review.value){response.bytes=application::encode_ftn_retention_preview(*review.value);if(response.bytes.empty())return response;response.total=static_cast<std::uint32_t>(response.bytes.size());response.revision=c.revision;response.code=application::FtnConfigurationCode::ok;}
            else response.code=(review.status.code==application::StorageErrorCode::backpressure||review.status.code==application::StorageErrorCode::busy)?application::FtnConfigurationCode::busy:review.status.code==application::StorageErrorCode::conflict?application::FtnConfigurationCode::conflict:application::FtnConfigurationCode::unavailable;
            return response;
        });
    return ftn_publisher_->handle(request,clock_.monotonic_millis());
}
