    else if(action=="setup_finish"){
        require(record.empty());keys(secrets,{});keys(v,{"enabled","queue","hub","selected"});
        auto net=find_network(next.networks,network);require(net!=next.networks.end(),"ftn_network_removed");net->enabled=flag(v.at("enabled"));next.setup_subscriptions.reset();
        const auto queue=flag(v.at("queue"));const auto hub=code(v.at("hub"),true);const auto& selected=v.at("selected");require(selected.is_array()&&selected.size()<=4096);
        if(queue){require(net->enabled&&!hub.empty()&&!selected.empty(),"ftn_setup_subscription_not_ready");app::FtnSetupSubscriptions command;command.network=net->code;std::set<std::string> unique;
            const auto link=std::find_if(next.links.begin(),next.links.end(),[&](const auto& l){return web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.code,hub);});require(link!=next.links.end()&&link->enabled&&!link->credentials.areafix.empty(),"ftn_setup_hub_not_ready");command.link=link->code;
            for(const auto& item:selected){const auto name=tag(item);require(unique.insert(name).second,"ftn_setup_duplicate_tag");const auto echo=std::find_if(next.message_echoes.begin(),next.message_echoes.end(),[&](const auto& e){return web_ftn_same_id(e.network,network)&&web_ftn_same_id(e.tag,name);});require(echo!=next.message_echoes.end(),"ftn_setup_echo_removed");require(std::any_of(next.permissions.begin(),next.permissions.end(),[&](const auto& p){return web_ftn_same_id(p.network,network)&&web_ftn_same_id(p.link,hub)&&web_ftn_same_id(p.tag,name)&&!p.file_echo&&p.receive;}),"ftn_setup_receive_required");command.tags.push_back(echo->tag);}
            next.setup_subscriptions=std::move(command);
        }else require(hub.empty()&&selected.empty(),"ftn_setup_no_implicit_subscription");
    }
    else if(action=="echo_presentation"){
        require(record.empty());keys(secrets,{});keys(v,{"selected","real_name","ansi","pipe"});const auto& selected=v.at("selected");require(selected.is_array()&&!selected.empty()&&selected.size()<=4096);
        require(!v.at("real_name").is_null()||!v.at("ansi").is_null()||!v.at("pipe").is_null(),"ftn_setup_no_fields_selected");std::set<std::string> unique;
        for(const auto& item:selected){const auto name=tag(item);require(unique.insert(name).second);auto echo=std::find_if(next.message_echoes.begin(),next.message_echoes.end(),[&](const auto& e){return web_ftn_same_id(e.network,network)&&web_ftn_same_id(e.tag,name);});require(echo!=next.message_echoes.end(),"ftn_setup_echo_removed");
            if(!v.at("real_name").is_null())echo->require_real_name=flag(v.at("real_name"));
            if(!v.at("ansi").is_null())echo->ansi_colors=flag(v.at("ansi"));
            if(!v.at("pipe").is_null()){const auto pipe=flag(v.at("pipe"));echo->update_local_permissions|=echo->local_echo&&echo->pipe_colors!=pipe;echo->pipe_colors=pipe;}
        }
    }
    else if(action=="echo_clone"){
        require(record.empty());keys(secrets,{});
        keys(v,{"selected","copy_count","maximum_messages","copy_age","maximum_age_days","copy_access","access_level","copy_read","read_level","copy_post","post_level"});
        const auto& selected=v.at("selected");require(selected.is_array()&&!selected.empty()&&selected.size()<=4096);
        std::vector<std::string> targets;std::set<std::string> unique;
        for(const auto& item:selected){const auto name=tag(item);require(unique.insert(name).second);targets.push_back(name);}
        app::FtnEchoBulkOptions options;options.copy_count=flag(v.at("copy_count"));options.copy_age=flag(v.at("copy_age"));
        if(options.copy_count)options.maximum_messages=limit(v.at("maximum_messages"),999999999);
        if(options.copy_age)options.maximum_age_days=limit(v.at("maximum_age_days"),65535);
        if(flag(v.at("copy_access")))options.access=static_cast<std::uint16_t>(integer(v.at("access_level"),1,999));
        if(flag(v.at("copy_read")))options.reading=static_cast<std::uint16_t>(integer(v.at("read_level"),1,999));
        if(flag(v.at("copy_post")))options.posting=static_cast<std::uint16_t>(integer(v.at("post_level"),1,999));
        const auto result=app::apply_ftn_echo_options(next,network,targets,options);
        require(result.succeeded()&&result.changed>0,"ftn_clone_selection_invalid");
    }
    else if(action=="echo_add"||action=="echo_save"){
        require(find_network(next.networks,network)!=next.networks.end(),"ftn_network_removed");keys(secrets,{});
        keys(v,{"tag","description","local_echo","store_locally","require_real_name","access_level","read_level","post_level","local_name","update_local_name","pipe_colors","update_local_permissions","maximum_messages","maximum_age_days","update_local_retention","ansi_colors"});
        auto found=std::find_if(next.message_echoes.begin(),next.message_echoes.end(),[&](const auto& e){return web_ftn_same_id(e.network,network)&&web_ftn_same_id(e.tag,record);});
        if(action=="echo_add"){require(record.empty());app::FtnMessageEcho e;e.network=network;next.message_echoes.push_back(std::move(e));found=std::prev(next.message_echoes.end());}
        else require(found!=next.message_echoes.end(),"ftn_record_removed");
        auto& e=*found;const auto new_tag=tag(v.at("tag"));require(!e.local_echo||web_ftn_same_id(new_tag,e.tag),"ftn_saved_echo_tag_fixed");if(!web_ftn_same_id(e.tag,new_tag))e.tag=new_tag;
        e.description=text(v.at("description"),512,true);e.local_echo=identity(v.at("local_echo"),true);e.store_locally=flag(v.at("store_locally"));e.require_real_name=flag(v.at("require_real_name"));
        e.access_level=static_cast<std::uint16_t>(integer(v.at("access_level"),1,999));e.read_level=static_cast<std::uint16_t>(integer(v.at("read_level"),1,999));e.post_level=static_cast<std::uint16_t>(integer(v.at("post_level"),1,999));
        e.local_name=text(v.at("local_name"),64,true);e.update_local_name=flag(v.at("update_local_name"));e.pipe_colors=flag(v.at("pipe_colors"));e.update_local_permissions=flag(v.at("update_local_permissions"));
        e.maximum_messages=limit(v.at("maximum_messages"),999999999);e.maximum_age_days=limit(v.at("maximum_age_days"),65535);e.update_local_retention=static_cast<std::uint8_t>(integer(v.at("update_local_retention"),0,3));
        e.ansi_colors=v.at("ansi_colors").is_null()?std::optional<bool>{}:std::optional<bool>{flag(v.at("ansi_colors"))};
        app::link_new_ftn_echoes_to_hub(current,next,network);
    }else if(action=="file_add"||action=="file_save"){
        require(find_network(next.networks,network)!=next.networks.end(),"ftn_network_removed");keys(secrets,{});keys(v,{"tag","description","file_area"});
        auto found=std::find_if(next.file_echoes.begin(),next.file_echoes.end(),[&](const auto& e){return web_ftn_same_id(e.network,network)&&web_ftn_same_id(e.tag,record);});
        if(action=="file_add"){require(record.empty());app::FtnFileEcho e;e.network=network;next.file_echoes.push_back(std::move(e));found=std::prev(next.file_echoes.end());}
        else require(found!=next.file_echoes.end(),"ftn_record_removed");
        const auto new_tag=tag(v.at("tag"));if(!web_ftn_same_id(found->tag,new_tag))found->tag=new_tag;found->description=text(v.at("description"),512,true);found->file_area=identity(v.at("file_area"));
    }else if(action=="route_add"||action=="route_save"){
        require(find_network(next.networks,network)!=next.networks.end(),"ftn_network_removed");keys(secrets,{});keys(v,{"destination","via_link","flavor"});
        auto found=std::find_if(next.routes.begin(),next.routes.end(),[&](const auto& r){return web_ftn_same_id(r.network,network)&&r.destination==record;});
        if(action=="route_add"){require(record.empty());app::FtnRoute r;r.network=network;next.routes.push_back(std::move(r));found=std::prev(next.routes.end());}
        else require(found!=next.routes.end(),"ftn_record_removed");
        const auto destination=app::parse_ftn_address(text(v.at("destination"),96));require(destination.has_value());found->destination=app::format_ftn_address(*destination);
        found->via_link=code(v.at("via_link"));found->flavor=static_cast<app::FtnMailFlavor>(integer(v.at("flavor"),0,2));
        require(std::any_of(next.links.begin(),next.links.end(),[&](const auto& l){return web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.code,found->via_link)&&app::ftn_outbound(l);}),"ftn_outgoing_connection_required");
    }else if(action=="permissions"){
        require(std::any_of(next.links.begin(),next.links.end(),[&](const auto& l){return web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.code,record);}),"ftn_record_removed");keys(secrets,{});keys(v,{"selected","receive","send","manage"});
        const auto receive=flag(v.at("receive")),send=flag(v.at("send")),manage=flag(v.at("manage"));const auto& selected=v.at("selected");require(selected.is_array()&&!selected.empty()&&selected.size()<=4096);
        std::set<std::pair<std::string,bool>> unique;
        for(const auto& item:selected){keys(item,{"tag","file_echo"});const auto echo=tag(item.at("tag"));const auto file=flag(item.at("file_echo"));require(unique.emplace(echo,file).second);require(!file||!manage,"ftn_areafix_message_echoes_only");
            auto p=std::find_if(next.permissions.begin(),next.permissions.end(),[&](const auto& entry){return web_ftn_same_id(entry.network,network)&&web_ftn_same_id(entry.link,record)&&web_ftn_same_id(entry.tag,echo)&&entry.file_echo==file;});
            if(p==next.permissions.end())next.permissions.push_back({network,record,echo,file,receive,send,manage});else{p->receive=receive;p->send=send;p->manage=manage;}
        }
    }else if(action=="network_remove"||action=="link_remove"||action=="echo_remove"||action=="file_remove"||action=="route_remove"){
        keys(v,{});keys(secrets,{});auto net=find_network(next.networks,network);require(net!=next.networks.end(),"ftn_network_removed");
        if(action=="network_remove"){
            require(record.empty());const auto member=[&](const auto& row){return web_ftn_same_id(row.network,network);};
            erase_if(next.links,member);erase_if(next.message_echoes,member);erase_if(next.file_echoes,member);erase_if(next.routes,member);erase_if(next.permissions,member);next.networks.erase(net);
        }else if(action=="link_remove"){
            const auto old=next.links.size();erase_if(next.links,[&](const auto& l){return web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.code,record);});require(old!=next.links.size(),"ftn_record_removed");
            if(web_ftn_same_id(net->default_link,record))net->default_link.clear();
            erase_if(next.routes,[&](const auto& r){return web_ftn_same_id(r.network,network)&&web_ftn_same_id(r.via_link,record);});erase_if(next.permissions,[&](const auto& p){return web_ftn_same_id(p.network,network)&&web_ftn_same_id(p.link,record);});
            for(auto& l:next.links)if(web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.forward_via_link,record)){l.forward_via_link.clear();l.forward_missing_echoes=false;}
        }else if(action=="route_remove"){
            const auto old=next.routes.size();erase_if(next.routes,[&](const auto& r){return web_ftn_same_id(r.network,network)&&r.destination==record;});require(old!=next.routes.size(),"ftn_record_removed");
        }else{
            const bool file=action=="file_remove";const auto remove=[&](auto& rows){const auto old=rows.size();erase_if(rows,[&](const auto& e){return web_ftn_same_id(e.network,network)&&web_ftn_same_id(e.tag,record);});require(old!=rows.size(),"ftn_record_removed");};
            if(file)remove(next.file_echoes);else remove(next.message_echoes);
            erase_if(next.permissions,[&](const auto& p){return web_ftn_same_id(p.network,network)&&web_ftn_same_id(p.tag,record)&&p.file_echo==file;});
        }
    }
