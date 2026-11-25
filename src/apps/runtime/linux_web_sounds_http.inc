    const auto sound_file_json=[](const rt::web::WebSoundFile& file){
        std::string encoded(sodium_base64_ENCODED_LEN(file.bytes.size(),sodium_base64_VARIANT_ORIGINAL),'\0');
        sodium_bin2base64(encoded.data(),encoded.size(),file.bytes.data(),file.bytes.size(),sodium_base64_VARIANT_ORIGINAL);encoded.resize(encoded.find('\0'));
        return Json{{"wave",encoded},{"duration_ms",file.duration_ms}};
    };
    api_get("/api/control/sounds",[&](const auto& request,auto& response){
        if(!request.params.empty())throw std::runtime_error("sound_query_invalid");
        const auto state=runtime.configuration();if(!state){json_response(response,{{"detail","Sound settings are unavailable until board setup is complete."}},409);return;}
        json_response(response,{{"revision",std::to_string(state->saved.configuration_revision)},{"paths",state->saved.event_sounds.paths},{"names",app::kSoundEventNames},{"activation",state->detail}});
    });
    api_post("/api/control/sounds",[&](const auto& request,auto& response){
        const auto value=parse(request,12000);require_keys(value,{"revision","paths"});
        const auto revision=value.at("revision").template get<std::string>();const auto expected=number(revision);
        if(std::to_string(expected)!=revision||!expected||expected==UINT64_MAX||!value.at("paths").is_array()||value.at("paths").size()!=app::kSoundEventCount)throw std::runtime_error("sound_assignments_invalid");
        const auto current=runtime.configuration();if(!current||current->saved.configuration_revision!=expected){json_response(response,{{"detail","Settings changed. Reload Event Sounds and review your assignments before saving."}},409);return;}
        auto candidate=current->saved;
        for(std::size_t i=0;i<app::kSoundEventCount;++i){const auto path=value.at("paths").at(i).template get<std::string>();
            if(!path.empty())try{(void)rt::web::read_web_sound(path);}catch(const std::exception&){json_response(response,{{"detail",std::string(app::kSoundEventNames[i])+": choose an owned local PCM WAV, 8/16-bit mono/stereo, 8-48 kHz, at most 30 seconds and 2 MiB. Links and writable shared files are refused. Nothing saved."}},400);return;}
            candidate.event_sounds.paths[i]=path;
        }
        candidate.configuration_revision=expected+1;const auto saved=runtime.save_configuration({expected,candidate});
        json_response(response,{{"ok",saved.code==Code::ok},{"detail",saved.detail},{"revision",std::to_string(saved.saved_revision)}},saved.code==Code::ok?200:409);
    });
    api_post("/api/control/sounds/preview",[&,sound_file_json](const auto& request,auto& response){
        const auto value=parse(request,2048);require_keys(value,{"path"});
        try{json_response(response,sound_file_json(rt::web::read_web_sound(value.at("path").template get<std::string>())));}
        catch(const std::exception&){json_response(response,{{"detail","WAV unavailable or unsupported. Use an owned local PCM WAV: 8/16-bit mono/stereo, 8-48 kHz, at most 30 seconds and 2 MiB. Links and writable shared files are refused."}},400);}
    });
    api_post("/api/control/sounds/poll",[&,terminal_owner](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"tab","epoch","after"});app::SoundPollRequest poll;
        poll.owner=rt::web::web_sound_owner(terminal_owner(request),value.at("tab").template get<std::string>());
        const auto epoch=value.at("epoch").template get<std::string>();const auto after=value.at("after").template get<std::string>();poll.after=number(after);
        if(std::to_string(poll.after)!=after||epoch.size()!=32||epoch.find_first_not_of("0123456789abcdef")!=epoch.npos)throw std::runtime_error("sound_cursor_invalid");
        if(sodium_hex2bin(poll.epoch.bytes.data(),poll.epoch.bytes.size(),epoch.data(),epoch.size(),nullptr,nullptr,nullptr)!=0)throw std::runtime_error("sound_epoch_invalid");
        const auto result=runtime.sound_poll(poll);Json events=Json::array();
        for(const auto& event:result.events)events.push_back({{"sequence",std::to_string(event.sequence)},{"event",static_cast<unsigned>(event.event)}});
        std::array<char,33> current{};sodium_bin2hex(current.data(),current.size(),result.epoch.bytes.data(),result.epoch.bytes.size());
        json_response(response,{{"owner",result.owner},{"epoch",current.data()},{"through",std::to_string(result.through)},{"events",events},{"paths",result.configuration.paths}});
    });
