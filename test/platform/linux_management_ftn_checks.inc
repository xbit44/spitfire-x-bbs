app::FtnConfigurationResponse ftn_exchange(rt::LinuxRuntimeHost& runtime,const app::FtnConfigurationRequest& request,bool catalog=false){
    const auto response=app::decode_ftn_configuration_response(exchange(runtime,
        catalog?app::LocalControlMessageType::ftn_hatch_catalog_request:app::LocalControlMessageType::ftn_configuration_request,
        app::encode_ftn_configuration_request(request),catalog?app::LocalControlMessageType::ftn_hatch_catalog_response:app::LocalControlMessageType::ftn_configuration_response));
    check(bool(response),"typed FTN catalog response decodes");return *response;
}
app::FtnConfiguration read_ftn(rt::LinuxRuntimeHost& runtime,host::LinuxSecureIdentifierGenerator& identifiers){
    app::FtnConfigurationRequest read;read.token=identifiers.generate().bytes;std::vector<std::uint8_t> bytes;
    for(;;){const auto response=ftn_exchange(runtime,read);check(response.code==app::FtnConfigurationCode::ok&&!response.bytes.empty(),"read pinned typed FTN configuration page");
        bytes.insert(bytes.end(),response.bytes.begin(),response.bytes.end());if(bytes.size()==response.total)break;read.offset=static_cast<std::uint32_t>(bytes.size());read.revision=response.revision;}
    read.operation=app::FtnConfigurationOperation::cancel;read.offset=0;check(ftn_exchange(runtime,read).code==app::FtnConfigurationCode::ok,"release pinned typed FTN reader");
    const auto config=app::decode_ftn_configuration(bytes);check(bool(config),"actual FTN configuration snapshot decodes");return *config;
}
app::FtnConfigurationResponse write_ftn(rt::LinuxRuntimeHost& runtime,const app::FtnConfiguration& config,app::FtnTransferToken token,bool cancel=false){
    const auto bytes=app::encode_ftn_configuration(config);check(!bytes.empty(),"valid native FTN draft encoding");
    app::FtnConfigurationRequest request;request.operation=app::FtnConfigurationOperation::begin;request.token=token;request.revision=config.revision;request.total=static_cast<std::uint32_t>(bytes.size());
    const auto begun=ftn_exchange(runtime,request);if(begun.code!=app::FtnConfigurationCode::ok)return begun;
    request.operation=app::FtnConfigurationOperation::append;
    for(std::size_t offset=0;offset<bytes.size();offset+=app::kFtnConfigurationPageBytes){request.offset=static_cast<std::uint32_t>(offset);request.bytes={bytes.begin()+offset,bytes.begin()+std::min(bytes.size(),offset+app::kFtnConfigurationPageBytes)};check(ftn_exchange(runtime,request).code==app::FtnConfigurationCode::ok,"typed FTN draft chunk accepted without live activation");}
    request.bytes.clear();request.offset=0;request.operation=cancel?app::FtnConfigurationOperation::cancel:app::FtnConfigurationOperation::commit;if(cancel)request.total=0;
    return ftn_exchange(runtime,request);
}
app::FtnRuntimeResponse ftn_status(rt::LinuxRuntimeHost& runtime,app::FtnRuntimeRequest request={}){
    const auto response=app::decode_ftn_runtime_response(exchange(runtime,app::LocalControlMessageType::ftn_runtime_request,app::encode_ftn_runtime_request(request),app::LocalControlMessageType::ftn_runtime_response));
    check(bool(response),"typed FTN Runtime response decodes");return *response;
}
void ftn_checks(rt::LinuxRuntimeHost& runtime,bool online,host::LinuxSecureIdentifierGenerator& identifiers){
    auto config=read_ftn(runtime,identifiers);const auto original=config;
    if(!online){
        int probe=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);check(probe>=0,"isolated FTN endpoint probe");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        socklen_t length=sizeof(address);check(::bind(probe,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&getsockname(probe,reinterpret_cast<sockaddr*>(&address),&length)==0,"select unused loopback FTN endpoint");config.settings.port=ntohs(address.sin_port);::close(probe);
        config.settings.mode=app::FtnMailerMode::native;config.settings.bind_address="127.0.0.1";config.settings.listener_enabled=true;
        app::FtnNetwork network;network.code="TEST";network.name="Stopped native FTN";network.address="2:100/2@test";network.enabled=true;config.networks.push_back(network);
    }else config.networks.front().name="Live native FTN";
    const auto token=identifiers.generate().bytes;const auto saved=write_ftn(runtime,config,token);check(saved.code==app::FtnConfigurationCode::ok&&saved.revision>config.revision,"typed FTN current revision commits");
    const auto stale=write_ftn(runtime,original,identifiers.generate().bytes);check(stale.code==app::FtnConfigurationCode::conflict,"stale FTN draft cannot overwrite current revision");
    app::FtnConfigurationRequest reconcile;reconcile.operation=app::FtnConfigurationOperation::reconcile;reconcile.token=token;check(ftn_exchange(runtime,reconcile).revision==saved.revision,"FTN commit receipt reconciles exact operation");
    const auto current=read_ftn(runtime,identifiers);auto discarded=current;discarded.networks.front().name="Discarded draft";
    check(write_ftn(runtime,discarded,identifiers.generate().bytes,true).code==app::FtnConfigurationCode::ok&&read_ftn(runtime,identifiers).networks.front().name==current.networks.front().name,"discarded FTN draft never changes durable/live configuration");
    app::FtnConfigurationRequest catalog;catalog.token=identifiers.generate().bytes;check(ftn_exchange(runtime,catalog,true).code==app::FtnConfigurationCode::ok,"typed hatch catalog available while stopped or running");catalog.operation=app::FtnConfigurationOperation::cancel;(void)ftn_exchange(runtime,catalog,true);
    auto state=ftn_status(runtime);if(online){for(unsigned i=0;i<300&&(!state.classic_listening||state.active_revision!=saved.revision);++i){std::this_thread::sleep_for(std::chrono::milliseconds(10));state=runtime.ftn_runtime({});}
        check(state.capable&&state.classic_listening&&state.active_revision==saved.revision&&state.classic_port==current.settings.port,"Runtime Start composes actual FTN listener and live typed save restarts same endpoint");
        app::FtnRuntimeRequest scan;scan.action=app::FtnRuntimeAction::scan;check(ftn_status(runtime,scan).code==app::FtnConfigurationCode::ok,"typed Scan reaches live sole processor");
        auto external=read_ftn(runtime,identifiers);external.settings.mode=app::FtnMailerMode::external_binkd;
        const auto changed=write_ftn(runtime,external,identifiers.generate().bytes);check(changed.code==app::FtnConfigurationCode::ok,"normal management saves External mode");
        for(unsigned i=0;i<300&&runtime.ftn_runtime({}).active_revision!=changed.revision;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto external_state=runtime.ftn_runtime({});check(external_state.mode==app::FtnMailerMode::external_binkd&&!external_state.classic_listening&&!external_state.tls_listening,"External management switch retires Native listeners");
        const auto epoch=runtime.status().runtime_epoch;
        check(runtime.ftn_runtime_checked(scan,epoch,changed.revision).code==app::FtnConfigurationCode::ok,"reviewed External Scan reaches actual processor");
        check(runtime.ftn_runtime_checked(scan,epoch,current.revision).code==app::FtnConfigurationCode::conflict,"stale reviewed Scan remains refused in External");
        app::FtnRuntimeRequest external_poll;external_poll.action=app::FtnRuntimeAction::poll;external_poll.network="TEST";external_poll.link="MISSING";
        check(runtime.ftn_runtime_checked(external_poll,epoch,changed.revision).code==app::FtnConfigurationCode::conflict,"External checked API refuses Native Poll");
        auto restored=read_ftn(runtime,identifiers);restored.settings=current.settings;check(write_ftn(runtime,restored,identifiers.generate().bytes).code==app::FtnConfigurationCode::ok,"normal management restores original Native settings");
    }else check(!state.classic_listening&&state.diagnostic=="ftn_bbs_stopped","saved FTN settings do not start stopped BBS");
    app::FtnRuntimeRequest poll;poll.action=app::FtnRuntimeAction::poll;poll.network="TEST";poll.link="MISSING";check(ftn_status(runtime,poll).code==app::FtnConfigurationCode::unavailable,"unconfigured Poll cannot report accepted work");
    app::FtnRuntimeRequest queues;queues.action=app::FtnRuntimeAction::queues;const auto queue=ftn_status(runtime,queues);check(queue.code==app::FtnConfigurationCode::ok&&queue.queues.has_value(),"typed queue query uses actual managed repository");
    check(runtime.bbs_owned()==online,"FTN edits and observation preserve explicit BBS running state");
}
