Json door_bulletin_catalog_page(const app::DoorBulletinCatalog& catalog,const app::DoorCatalog& doors,std::string_view token){
    require(!app::encode_door_bulletin_catalog(catalog).empty());Json entries=Json::array(),choices=Json::array();
    for(const auto& item:catalog.entries){const auto& d=item.definition;entries.push_back({{"id",d.id.value},{"title",d.title},{"enabled",d.enabled},{"source",static_cast<unsigned>(d.source)},{"door",d.door},{"path",d.relative_path},{"format",static_cast<unsigned>(d.format)},{"health",static_cast<unsigned>(item.health)}});}
    for(const auto& door:doors.doors)if(door.kind==app::DoorExecutionKind::local)choices.push_back({{"id",door.id},{"name",door.name}});
    return {{"revision",std::to_string(catalog.revision)},{"operation",std::string(token)},{"entries",entries},{"doors",choices}};
}
app::DoorBulletinChange door_bulletin_update(const Json& value){
    keys(value,{"operation","revision","action","definition"});app::DoorBulletinChange request;request.operation=operation(value.at("operation"));request.expected_revision=revision(value.at("revision"));const auto action=text(value.at("action"),16);require(action=="create"||action=="save"||action=="remove");request.create_only=action=="create";request.remove=action=="remove";
    const auto& fields=value.at("definition");if(request.remove){keys(fields,{"id"});request.definition.id={static_cast<std::uint16_t>(number(fields.at("id"),1,999))};}
    else{keys(fields,{"id","title","enabled","source","door","path","format"});auto& d=request.definition;d.id={static_cast<std::uint16_t>(number(fields.at("id"),1,999))};d.title=text(fields.at("title"),64);d.enabled=flag(fields.at("enabled"));d.source=static_cast<app::DoorBulletinSourceKind>(number(fields.at("source"),0,1));d.door=static_cast<app::DoorId>(number(fields.at("door"),0,999));d.relative_path=text(fields.at("path"),240,true);d.format=static_cast<app::GraphicsMode>(number(fields.at("format"),0,1));}
    require(app::valid_door_bulletin_change(request));return request;
}
Json door_bulletin_preview_page(const app::DoorBulletinPreview& preview){
    require(app::valid_door_bulletin_preview(preview));Json cells=Json::array();for(const auto& c:preview.cells)cells.push_back(Json::array({c.glyph,c.attribute}));return {{"id",preview.id.value},{"title",preview.title},{"available",preview.available},{"detail",preview.diagnostic},{"columns",preview.columns},{"rows",preview.rows},{"cells",cells}};
}
