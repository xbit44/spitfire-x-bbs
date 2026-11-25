    {
    Json query{{"group",nullptr},{"revision","9007199254740993"},{"after_order","-1"},{"after_id","0"}};
    const auto selected=web::message_catalog_query(query);check(selected.cursor.configuration_revision==9007199254740993LL&&selected.cursor.page_size==64&&!selected.group,"message catalog preserves exact revisions and bounded pagination");
    app::LocalMessageConfigurationSnapshot snapshot;snapshot.code=app::LocalAccountResultCode::ok;snapshot.catalog.revision=9007199254740993LL;
    snapshot.catalog.groups.push_back({{9007199254740995LL},"Exact group",17,3,10,true});snapshot.catalog.next=app::MessageCatalogCursor{snapshot.catalog.revision,17,9007199254740995LL,64};
    const auto page=web::message_catalog_page(snapshot,selected,"abcdef0123456789abcdef0123456789");
    check(page["groups"][0]["id"]=="9007199254740995"&&page["next"]["after_id"]=="9007199254740995"&&page["revision"]=="9007199254740993","message IDs and page cursors never round through JavaScript numbers");
    const auto next=web::message_catalog_query(page["next"]);check(next.cursor.after_order==17&&next.cursor.after_id==9007199254740995LL,"exact next cursor decoded");
    for(const auto& bad:std::vector<Json>{1,true,1.5,"01","-2","9223372036854775807","90071992547409930x"}){auto invalid=query;invalid["revision"]=bad;refuses([&]{web::message_catalog_query(invalid);});}
    auto invalid=query;invalid["owner"]=true;refuses([&]{web::message_catalog_query(invalid);});
    Json change{{"revision",page["revision"]},{"operation",page["operation"]},{"action","group"},{"entry",page["groups"][0]}};
    const auto group=web::message_catalog_update(change);check(group.group.id.value==9007199254740995LL&&group.group.order==17&&group.group.access==10&&group.group.revision==3,"typed group update binds exact stable ID and revision");
    change["entry"]["id"]="0";change["entry"]["revision"]="0";check(web::message_catalog_update(change).group.id.value==0,"new group uses explicit zero identity and revision");
    for(const auto& bad:std::vector<Json>{"", " spaced", "trailing ", "line\nbreak",std::string(65,'x')}){auto value=change;value["entry"]["name"]=bad;refuses([&]{web::message_catalog_update(value);});}
    invalid=change;invalid["entry"]["revision"]="1";refuses([&]{web::message_catalog_update(invalid);});
    invalid=change;invalid["entry"]["access"]=true;refuses([&]{web::message_catalog_update(invalid);});
    snapshot.catalog.groups.clear();snapshot.catalog.echoes.push_back({{21},{9007199254740995LL},7,"Local",5,9,{10,20,30},true,true,{std::nullopt,365}});
    auto echo_page=web::message_catalog_page(snapshot,selected,"abcdef0123456789abcdef0123456789");change["action"]="echo";change["entry"]=echo_page["echoes"][0];
    const auto echo=web::message_catalog_update(change);check(echo.echo.id.value==21&&echo.echo.group.value==9007199254740995LL&&echo.echo.conference_number==7&&!echo.echo.retention.maximum_count&&echo.echo.retention.maximum_age_days==365&&echo.presentation_mask==1&&echo.retention_mask==3,"echo edit preserves stable parent and explicit presentation and retention intent");
    for(const auto& bad:std::vector<Json>{0,-1,1000000000,true,"1000",1.5}){invalid=change;invalid["entry"]["maximum_count"]=bad;refuses([&]{web::message_catalog_update(invalid);});}
    invalid=change;invalid["entry"]["maximum_age"]=65536;refuses([&]{web::message_catalog_update(invalid);});
    invalid=change;invalid["entry"]["sequence"]="9";refuses([&]{web::message_catalog_update(invalid);});
    change["action"]="insert";change["entry"]["id"]="0";change["entry"]["revision"]="0";change["entry"]["number"]=0;change["entry"]["before"]="21";change["entry"]["before_revision"]="9";
    check(web::message_catalog_update(change).insert_before.value==21,"insert is bound to stable anchor and its revision");
    invalid=change;invalid["entry"]["id"]="22";refuses([&]{web::message_catalog_update(invalid);});
    change["action"]="up";change["entry"]={{"id","21"},{"revision","9"}};check(web::message_catalog_update(change).action==app::MessageConfigurationAction::move_echo_up,"move uses exact selected identity");
    invalid=change;invalid["action"]="delete_echo";refuses([&]{web::message_catalog_update(invalid);});
    change["action"]="clone";change["entry"]={{"source","21"},{"source_revision","9"},{"targets",Json::array({{{"id","22"},{"revision","3"}}})},{"permission_mask",5},{"retention_mask",2}};
    const auto clone=web::message_catalog_update(change);check(clone.permission_mask==5&&clone.retention_mask==2&&clone.clone_targets.size()==1&&clone.clone_targets[0].echo.value==22,"Clone Options changes only explicit fields and targets");
    invalid=change;invalid["entry"]["targets"].push_back(invalid["entry"]["targets"][0]);refuses([&]{web::message_catalog_update(invalid);});
    invalid=change;invalid["entry"]["targets"][0]["id"]="21";refuses([&]{web::message_catalog_update(invalid);});
    invalid=change;invalid["entry"]["permission_mask"]=8;refuses([&]{web::message_catalog_update(invalid);});
    invalid=change;invalid["entry"]["retention_mask"]=4;refuses([&]{web::message_catalog_update(invalid);});
    change["entry"]["targets"]=Json::array();for(int i=0;i<999;++i)change["entry"]["targets"].push_back({{"id",std::to_string(i+100)},{"revision","1"}});
    check(web::message_catalog_update(change).clone_targets.size()==999,"full shared 999-target Clone Options limit accepted");change["entry"]["targets"].push_back({{"id","9999"},{"revision","1"}});refuses([&]{web::message_catalog_update(change);});
    }
