    {
    Json q{{"group",nullptr},{"revision","0"},{"after_id","0"}};
    check(web::file_catalog_query(q).part==app::FileConfigurationPart::groups,"file groups default page");
    q["group"]="9007199254740993";q["revision"]="9007199254740993";check(web::file_catalog_query(q).group.value==9007199254740993LL,"file IDs exact beyond JS precision");
    for(const auto& bad:std::vector<Json>{"01","-1",1,1.5,true,nullptr,"9223372036854775807"}){auto v=q;v["revision"]=bad;refuses([&]{web::file_catalog_query(v);});}
    q["revision"]="0";q["after_id"]="1";refuses([&]{web::file_catalog_query(q);});
    Json group{{"id","0"},{"revision","1"},{"name","Files"},{"order","0"},{"access",1},{"enabled",true}};
    Json request{{"revision","9007199254740993"},{"operation","0123456789abcdef0123456789abcdef"},{"action","group"},{"entry",group}};
    check(web::file_catalog_update(request).expected_catalog_revision==9007199254740993LL,"new file group uses shared change");
    for(const auto* key:{"deletion_operation","delete_disk_files","owner","deletion_items"}){auto v=request;v["entry"][key]=true;refuses([&]{web::file_catalog_update(v);});}
    for(const auto* action:{"preview_group_delete","begin_group_delete","finish_group_delete","delete","policy"}){auto v=request;v["action"]=action;refuses([&]{web::file_catalog_update(v);});}
    Json area{{"id","0"},{"revision","1"},{"group","1"},{"number",0},{"kind",0},{"name","Downloads"},{"description",""},{"order","0"},{"access",1},{"upload",1},{"download",1},{"uploads_enabled",false},{"downloads_sysop_only",false},{"enabled",true},{"own_description_edit",false},{"backup_contents",false},{"ratio_uploads",true},{"ratio_downloads",true},{"sysop_review",false},{"maximum_upload_bytes",nullptr},{"storage_path","Files/Downloads"}};
    request["action"]="area";request["entry"]=area;check(!web::file_catalog_update(request).area.area.maximum_upload_bytes,"file area null cap inherits global");
    for(const auto& bad:std::vector<Json>{0,-1,1073741825ULL,1.5,"10",true}){auto v=request;v["entry"]["maximum_upload_bytes"]=bad;refuses([&]{web::file_catalog_update(v);});}
    for(const auto* path:{"","../Data","Files/../Data","Files/Downloads/","/","//host/path"}){auto v=request;v["entry"]["storage_path"]=path;refuses([&]{web::file_catalog_update(v);});}
    auto private_area=request;private_area["entry"]["kind"]=2;refuses([&]{web::file_catalog_update(private_area);});
    private_area["entry"]["id"]="2";private_area["entry"]["number"]=2;private_area["entry"]["downloads_sysop_only"]=true;check(web::file_catalog_update(private_area).area.area.kind==app::FileAreaKind::private_sysop,"existing protected kind requires Sysop downloads");
    for(const auto* action:{"insert_before","insert_after"}){auto v=request;v["action"]=action;v["entry"]["anchor"]="9007199254740993";v["entry"]["anchor_revision"]="2";check(web::file_catalog_update(v).anchor.value==9007199254740993LL,"insert binds exact anchor");v["entry"]["id"]="8";refuses([&]{web::file_catalog_update(v);});}
    app::LocalFileConfigurationSnapshot snap;snap.code=app::LocalAccountResultCode::ok;snap.part=app::FileConfigurationPart::groups;snap.policy.catalog_revision=9007199254740993LL;snap.groups.push_back({{9007199254740993LL},"Large ID",0,1,1,true});
    app::LocalFileConfigurationQuery selected;selected.part=app::FileConfigurationPart::groups;
    const auto page=web::file_catalog_page(snap,selected,"0123456789abcdef0123456789abcdef");check(page["groups"][0]["id"]=="9007199254740993"&&page["revision"]=="9007199254740993","file catalog output preserves exact integer strings");
    }
