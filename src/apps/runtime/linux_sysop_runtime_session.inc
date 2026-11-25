// This controller is created only by the authenticated caller worker. Every
// service rechecks the exact durable session and completed SY challenge.
const auto sysop_menu=[&]() -> std::optional<std::string> {
    using namespace app;
    const auto identity=chat_identity;
    const auto authorized=[this,identity]{return sysop_actor(identity).has_value();};
    if(!authorized())return "Sysop Utilities access denied.";
    const auto actor=sysop_actor(identity);if(!actor)return "Sysop Utilities access changed.";
    const auto root=this->root;
    auto service=std::make_shared<host::LinuxFileAdministration>(database(),root,store.file_bindings(),*actor,[this,identity]{return sysop_actor(identity);});
    struct ReleaseFiles {Impl& runtime;std::shared_ptr<host::LinuxFileAdministration> files;~ReleaseFiles(){try{runtime.sysop_work.submit([f=std::move(files)]{f->stop();});}catch(...){if(files)files->stop();}}} release_files{*this,service};
    SysopSessionPorts sp{terminal};
    sp.authorized=[&]{return caller.tick()&&authorized();};
    sp.ansi=[&]{return ready.graphics==GraphicsMode::ansi;};
    sp.hotkeys=[&]{const auto v=database().menu_preferences(identity.account);return v&&v.value&&v.value->preferences.hotkey;};
    sp.rows=[&]{return assignment->snapshot().terminal_rows;};sp.columns=[&]{return assignment->snapshot().terminal_columns;};
    sp.write=[&](std::string_view text){return authorized()&&write_all(guarded,text);};
    sp.newsletter=[&]{
        NewsletterFileStore file(root);
        return edit_sysop_newsletter({terminal,sp.authorized,sp.ansi,sp.columns,sp.rows,sp.write,
            [&]{return guarded.read_editor_byte(terminal);},[&](bool cr){terminal.finish_editor_key(cr);},
            [&]{return file.load();},[&](const auto& original,std::string_view bytes){return file.save(original,bytes,sp.authorized);}});
    };
    sp.wait=[&](const std::function<bool()>& finished){
        struct Restore{MenuModalStream& stream;~Restore(){stream.preserve_poll_timeouts(false);}} restore{guarded};guarded.preserve_poll_timeouts(true);
        while(!finished()){
            if(!caller.tick()||!authorized())return false;
            const auto input=terminal.read_editor_byte();
            if(input.outcome==TerminalOutcome::timed_out)continue;
            if(input.outcome!=TerminalOutcome::completed)return false;
            if(input.byte&&(*input.byte=='q'||*input.byte=='Q'||*input.byte==27)){terminal.finish_editor_key(false);return false;}
        }return true;
    };
    sp.artwork=[&]{
        if(!authorized()||terminal.clear_screen()!=TerminalOutcome::completed)return false;
        const auto snapshot=ports.snapshot();if(!snapshot)return false;
        TrustedDisplaySelectionRequest request;request.identity=TrustedDisplayIdentity::menu_sysop;request.graphics=ready.graphics;const auto current_account=database().find_account_by_id(identity.account);if(!current_account||!current_account.value||!current_account.value->security_level)return false;request.authenticated_security_level=*current_account.value->security_level;request.context=snapshot->context;
        const auto art=ports.artwork(request);
        if(!art.available()||!art.program)return false;
        const auto program=trim_menu_artwork_padding(art.program->terminal_program);
        const auto page_length=static_cast<std::uint16_t>(std::clamp<unsigned>(sp.rows()>1?sp.rows()-1:10,10,24));
        return !program.operations.empty()&&
            terminal.present_display({"sysop.artwork",&program,{true,page_length}})==TerminalOutcome::completed&&
            sp.write(sp.ansi()?"\x1b[0m\r\n":"\r\n");
    };
    sp.accounts=[&](const LocalAccountQuery& query){
        if(!authorized())return LocalAccountSnapshot{LocalAccountResultCode::unauthorized};
        const auto configuration=config;
        RuntimeSysopAccountAuthority grant{[this,identity]()->std::optional<AccountId>{const auto current=sysop_actor(identity);return current?std::optional<AccountId>{current->account}:std::nullopt;},identity.session};
        LocalAccountAdministration administration(database(),database(),*store.authentication(),*this,store.identifiers(),clock,configuration,std::move(grant));
        auto result=administration.snapshot(query);result.online=query.account_id&&registry->account_online(query.account_id);return result;
    };
    sp.account_change=[&](LocalAccountRequest& request){
        if(!authorized()){erase_local_account_secrets(request);return LocalAccountResult{LocalAccountResultCode::unauthorized};}
        try{
            auto future=sysop_work.submit([this,identity,request,config]() mutable {
                struct Erase{LocalAccountRequest& request;~Erase(){erase_local_account_secrets(request);}} erase{request};
                const auto configuration=config;
                RuntimeSysopAccountAuthority grant{[this,identity]()->std::optional<AccountId>{const auto current=sysop_actor(identity);return current?std::optional<AccountId>{current->account}:std::nullopt;},identity.session};
                LocalAccountAdministration administration(database(),database(),*store.authentication(),*this,store.identifiers(),clock,configuration,std::move(grant));
                CancellationSource cancellation;
                return administration.execute(request,cancellation.token());
            });erase_local_account_secrets(request);
            if(!sp.wait([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;}))return LocalAccountResult{LocalAccountResultCode::storage_failure};
            return future.get();
        }catch(...){erase_local_account_secrets(request);return LocalAccountResult{LocalAccountResultCode::storage_failure};}
    };
    sp.online=[&]{struct Restore{bool& flag;~Restore(){flag=false;}} restore{sysop_online_context};sysop_online_context=true;chat.safe(identity,true);const auto ok=chat_session.online();chat.safe(identity,false);return ok;};
    sp.messages=[&]{
        const auto current_actor=[&]()->std::optional<MessageActor>{return sysop_actor(identity);};
        const auto area=select_local_message_echo({database(),message_terminal,current_actor,static_cast<std::size_t>(std::max(1U,sp.rows()>8?sp.rows()-8:1U))});
        if(area.outcome==MessageSelectionOutcome::cancelled)return true;
        if(!area.selection)return false;
        MessageReaderPorts reader{database(),message_terminal,current_actor,{},message_date,{},24,true,{},[&]{return store.identifiers().generate();},[&]{return MessageEditorDimensions{sp.ansi(),sp.columns(),sp.rows()};},[]{return std::uint8_t{7};},true};
        reader.legacy_pipe_colors=[&]{return std::atomic_load(&live_settings)->configuration.menus.legacy_message_pipe_colors;};
        MessageQueryOptions options;options.group=area.selection->group.id;options.echo=area.selection->echo.id;
        return read_local_messages(reader,options,true)!=MessageReaderOutcome::failed;
    };
    sp.file_configuration=[&]()->std::optional<FileConfiguration>{const auto current=sysop_actor(identity);if(!current)return {};const auto config=database().file_configuration({false,*current});return config&&config.value?config.value:std::nullopt;};
    sp.files=[&](const FileManagementReadRequest& request){const auto current=sysop_actor(identity);return current?LocalFileManagementQueries(database(),*current).snapshot(request):FileManagementReadSnapshot{LocalAccountResultCode::unauthorized};};
    sp.select_all_files=[&](const FileManagementQuery& query)->std::optional<std::vector<ManagedFile>>{
        auto cancel=std::make_shared<std::atomic_bool>(false);
        try{auto future=sysop_work.submit([this,identity,query,cancel]()->std::optional<std::vector<ManagedFile>>{
            std::vector<ManagedFile> result;FileManagementReadRequest request;request.filter=query;
            for(unsigned page=0;page<25000;++page){if(cancel->load())return {};const auto current=sysop_actor(identity);if(!current)return {};
                const auto read=LocalFileManagementQueries(database(),*current).snapshot(request);if(read.code!=LocalAccountResultCode::ok)return {};
                result.insert(result.end(),read.records.begin(),read.records.end());if(!read.next.value)return result;if(read.next.value<=request.after.value)return {};request.after=read.next;
            }return {};
        });if(!sp.wait([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;})){cancel->store(true);return {};}return future.get();}catch(...){return {};}
    };
    sp.file_start=[service](const FileManagementTask& task){return service->start(task);};
    sp.file_status=[service](Identifier128 job){return service->status(job);};
    sp.file_cancel=[service](Identifier128 job){return service->cancel(job);};
    sp.identifier=[&]{return store.identifiers().generate();};
    const auto ftn_configuration=[&]()->std::optional<FtnConfiguration>{const auto current=sysop_actor(identity);if(!current)return {};const auto config=database().ftn_configuration({false,*current});return config&&config.value?config.value:std::nullopt;};
    sp.networks=[&]{std::vector<SysopNamedChoice> choices;const auto config=ftn_configuration();if(config)for(const auto& network:config->networks)choices.push_back({network.code,network.name,network.enabled});return choices;};
    sp.links=[&](std::string_view network){std::vector<SysopNamedChoice> choices;const auto config=ftn_configuration();if(config)for(const auto& link:config->links)if(link.network==network)choices.push_back({link.code,link.name,link.enabled&&ftn_outbound(link)});return choices;};
    sp.poll=[&](std::string_view network,std::string_view link){
        const auto current=sysop_actor(identity);const auto mailer=std::atomic_load(&ftn_engine);
        if(!current||!mailer)return std::string("Poll unavailable; no request accepted.");
        const auto policy=database().ftn_configuration({false,*current});
        if(!policy||!policy.value||!sysop_actor(identity))return std::string("Sysop access changed; no request accepted.");
        const auto& config=*policy.value;
        const auto n=std::find_if(config.networks.begin(),config.networks.end(),[&](const auto& v){return v.code==network&&v.enabled;});
        const auto l=std::find_if(config.links.begin(),config.links.end(),[&](const auto& v){return v.network==network&&v.code==link&&v.enabled&&ftn_outbound(v);});
        if(n==config.networks.end()||l==config.links.end())return std::string("Network or link changed; refresh before polling.");
        return n->name+" / "+l->name+": "+(mailer->poll(network,link,false)?"Poll requested. This is not a completed session or delivery confirmation.":"Poll was not accepted; the link may be busy or unavailable.");
    };
    sp.ftn_view=[&](std::string_view network,SysopFtnView kind,std::uint64_t page){
        const auto height=sp.rows();auto cancel=std::make_shared<std::atomic_bool>(false);
        try{auto future=sysop_work.submit([this,identity,height,network=std::string(network),kind,page,cancel]{
            const LinuxSysopFtnActivityRead activity=[this]()->std::optional<std::vector<LinuxSysopFtnActivity>>{
                const auto mailer=std::atomic_load(&ftn_engine);if(!mailer)return {};
                std::vector<LinuxSysopFtnActivity> result;for(const auto& item:mailer->snapshot().activity)result.push_back({item.network,item.link,item.message});return result;
            };
            return read_linux_sysop_ftn(database(),[&]()->std::optional<MessageActor>{return cancel->load()?std::nullopt:sysop_actor(identity);},activity,network,kind,page,height);
        });
        if(!sp.wait([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;})){cancel->store(true);SysopTextPage out;out.notice="FTN read cancelled or access changed.";return out;}return future.get();
        }catch(...){SysopTextPage out;out.notice="FTN view unavailable.";return out;}
    };
    sp.log=[&](SysopLog source,std::optional<std::uint64_t> page){
        auto cancel=std::make_shared<std::atomic_bool>(false);const auto height=sp.rows(),width=sp.columns();
        try{auto future=sysop_work.submit([root,source,page,height,width,authorized,cancel]{return read_linux_sysop_log(root,source,page,height,width,[&]{return !cancel->load()&&authorized();});});
            if(!sp.wait([&]{return future.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;})){cancel->store(true);SysopTextPage out;out.notice="Log read cancelled or access changed.";return out;}return future.get();
        }catch(...){SysopTextPage out;out.notice="Current log unavailable.";return out;}
    };
    const auto ok=SysopCallerSession(sp).run();
    if(!caller.refresh_access())return {};
    return ok?std::string{}:std::string("Sysop Utilities closed; refresh your access and selected records before continuing.");
};
