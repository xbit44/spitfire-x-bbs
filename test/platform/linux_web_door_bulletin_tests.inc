    {
    const auto before=checks;
    app::DoorBulletinCatalog bulletins;bulletins.revision=9007199254740993LL;
    app::DoorBulletinEntry first;first.definition.id={1};first.definition.title="Door news";first.definition.revision=1;
    auto last=first;last.definition.id={999};last.definition.title="Last scoreboard";
    bulletins.entries={first,last};
    app::DoorCatalog doors;app::DoorDefinition local;local.id=256;local.name="Local scoreboard writer";local.kind=app::DoorExecutionKind::local;
    app::DoorDefinition remote;remote.id=257;remote.name="Remote game";doors.doors={local,remote};
    const std::string operation(32,'a');
    const auto catalog=web::door_bulletin_catalog_page(bulletins,doors,operation);
    check(catalog.at("revision")=="9007199254740993"&&catalog.at("operation")==operation,"Door Bulletin browser preserves decimal revision and operation");
    check(catalog.at("entries").size()==2&&catalog.at("entries")[0]["id"]==1&&catalog.at("entries")[1]["id"]==999,"browser retains permanent IDs and gaps");
    check(catalog.at("doors").size()==1&&catalog.at("doors")[0]["id"]==256,"only local Door sources offered, not remote gateway programs");
    const Json definition{{"id",999},{"title","Door news"},{"enabled",true},{"source",0},{"door",0},{"path",""},{"format",0}};
    const Json original{{"operation",operation},{"revision","9007199254740993"},{"action","create"},{"definition",definition}};
    auto parsed=web::door_bulletin_update(original);
    check(parsed.create_only&&!parsed.remove&&parsed.definition.id.value==999&&parsed.expected_revision==9007199254740993LL,"create preserves full ID/revision and explicit operation");
    auto value=original;value["action"]="save";parsed=web::door_bulletin_update(value);
    check(!parsed.create_only&&!parsed.remove,"save cannot silently become create");
    for(const auto& bad:std::vector<Json>{0,-1,1000,"1",true,1.5,nullptr}){
        value=original;value["definition"]["id"]=bad;refuses([&]{web::door_bulletin_update(value);});
    }
    for(const auto& bad:std::vector<Json>{1,"01","0","-1","9007199254740993x"}){
        value=original;value["revision"]=bad;refuses([&]{web::door_bulletin_update(value);});
    }
    value=original;value["unexpected"]=true;refuses([&]{web::door_bulletin_update(value);});
    value=original;value["definition"]["unexpected"]=true;refuses([&]{web::door_bulletin_update(value);});
    value=original;value["definition"]["enabled"]="true";refuses([&]{web::door_bulletin_update(value);});
    value=original;value["definition"]["source"]=2;refuses([&]{web::door_bulletin_update(value);});
    value=original;value["action"]="rename";refuses([&]{web::door_bulletin_update(value);});
    value=original;value["definition"]["path"]="unrelated.txt";refuses([&]{web::door_bulletin_update(value);});
    value=original;value["definition"]["source"]=1;value["definition"]["door"]=256;value["definition"]["path"]="Scores/RESULTS.ANS";value["definition"]["format"]=1;
    const auto generated=value;parsed=web::door_bulletin_update(generated);
    check(parsed.definition.door==256&&parsed.definition.relative_path=="Scores/RESULTS.ANS"&&parsed.definition.format==app::GraphicsMode::ansi,"generated source retains Door ID and relative explicit format");
    for(const auto* path:{"../OUT.TXT","/OUT.TXT","C:/OUT.TXT","Scores/x:stream",""}){
        value=generated;value["definition"]["path"]=path;refuses([&]{web::door_bulletin_update(value);});
    }
    value=original;value["action"]="remove";value["definition"]={{"id",999}};
    parsed=web::door_bulletin_update(value);check(parsed.remove&&!parsed.create_only&&parsed.definition.id.value==999,"remove targets permanent ID only");
    value["definition"]["path"]="unrelated.txt";refuses([&]{web::door_bulletin_update(value);});
    app::DoorBulletinPreview preview;preview.id={1};preview.title="Preview";preview.available=true;preview.rows=25;preview.cells.assign(2000,{65,7});
    const auto page=web::door_bulletin_preview_page(preview);
    check(page.at("columns")==80&&page.at("rows")==25&&page.at("cells").size()==2000&&page.at("cells")[0]==Json::array({65,7}),"browser receives bounded inert glyph/attribute cells");
    preview.cells.pop_back();refuses([&]{web::door_bulletin_preview_page(preview);});
    preview.available=false;preview.rows=0;preview.cells.clear();preview.diagnostic="Source unavailable";
    check(web::door_bulletin_preview_page(preview).at("cells").empty(),"unavailable preview exposes no old cells");
    std::cout<<"PASS "<<(checks-before)<<" Door Bulletin typed browser form checks\n";
    }
