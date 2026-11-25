// Preview has no input, action, transport or persistence authority. It uses
// the same bounded artwork selector and menu renderer as a real caller.
Json menu_preview(const Json& request,const app::AlphaConfigurationSnapshot& snapshot,app::ITrustedDisplayAssetReader* reader){
    require(request.is_object()&&request.size()==5&&request.contains("values")&&request.contains("menu")&&request.contains("ansi")&&request.contains("level")&&request.contains("sysop")&&request.at("values").is_object(),"preview_request_invalid");
    auto draft=snapshot.saved;auto controls=request.at("values");
    require(controls.contains("artwork")&&controls.at("artwork").is_boolean(),"preview_artwork_invalid");
    const bool artwork=controls.at("artwork").get<bool>();controls.erase("artwork");
    (void)fields("general.displays_menus",draft,&controls);
    require(app::validate_operational_configuration(draft).valid(),"preview_colors_invalid");
    const auto& menu=request.at("menu");const auto& level=request.at("level");
    require(menu.is_number_integer()&&menu>=Json(0)&&menu<=Json(13)&&menu!=Json(3)&&menu!=Json(4),"preview_menu_invalid");
    require(level.is_number_integer()&&level>=Json(1)&&level<=Json(999)&&request.at("ansi").is_boolean()&&request.at("sysop").is_boolean(),"preview_access_invalid");
    app::MenuSessionSnapshot sample;sample.access={level.get<std::uint16_t>(),request.at("sysop").get<bool>(),1};
    sample.configuration_revision=1;sample.preferences={1,{},1,1};sample.graphics=request.at("ansi").get<bool>()?app::GraphicsMode::ansi:app::GraphicsMode::ascii;
    sample.default_color=draft.menus.text_color;sample.input_color=draft.menus.input_color;sample.lightbars_enabled=draft.menus.lightbars_enabled;
    sample.selected_foreground=draft.menus.selected_foreground;sample.selected_background=draft.menus.selected_background;sample.compact_footer=draft.menus.compact_footer;
    auto& context=sample.context;context.bbs_name=draft.bbs_name;context.sysop="Preview Sysop";context.version=SFX_PRODUCT_DISPLAY_VERSION;
    context.date="2026-09-27";context.time="12:34:56";context.datetime="2026-09-27 12:34:56";context.node="1";context.nodes=std::to_string(draft.logical_nodes);context.transport="Preview";
    context.columns="80";context.rows="25";context.name="Preview Caller";context.alias="Preview Caller";context.first_name="Preview";context.security_level=std::to_string(sample.access.security_level);context.time_on="60";context.time_left="Unlimited";
    app::ControlPanelTerminalMirror mirror;std::string detail="Built-in menu. Sample values only; no caller actions.";
    app::MenuSessionPorts ports;ports.snapshot=[&]{return std::optional<app::MenuSessionSnapshot>{sample};};
    ports.artwork=[&](const app::TrustedDisplaySelectionRequest& selection){
        if(!artwork)return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::not_found,{}};
        if(!reader){detail="No usable matching artwork; built-in fallback.";return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::not_found,{}};}
        app::TrustedDisplaySelector selector(*reader);auto selected=selector.select(selection);
        detail=selected.available()?"Selected: "+selected.program->selected_logical_asset+". Sample values only; bell and pause disabled.":"No usable matching artwork; built-in fallback.";return selected;
    };
    ports.write=[&](std::string_view bytes){mirror.consume(bytes);return true;};
    ports.display=[&](const app::TerminalDisplayProgram& program){for(const auto& op:program.operations)switch(op.kind){
        case app::TerminalDisplayOperationKind::cp437_text:case app::TerminalDisplayOperationKind::ansi_sequence:mirror.consume(op.bytes);break;
        case app::TerminalDisplayOperationKind::logical_newline:case app::TerminalDisplayOperationKind::paging_newline:mirror.consume("\r\n");break;
        case app::TerminalDisplayOperationKind::clear_screen:mirror.clear();break;default:break;
    }return true;};
    ports.read=[](app::MenuInputDecoder&,const std::function<bool()>&)->std::optional<app::MenuInput>{return {};};
    ports.save_preferences=[](const app::UpdateMenuPreferenceRequest&){return false;};ports.action=[](app::MenuAction)->std::optional<std::string>{return {};};ports.utc_now=[]{return std::int64_t{0};};
    (void)app::run_menu_session(ports,static_cast<app::MenuId>(menu.get<unsigned>()));
    Json cells=Json::array();for(const auto& cell:mirror.snapshot().cells)cells.push_back(Json::array({cell.character,cell.attribute}));
    return {{"columns",80},{"rows",25},{"cells",cells},{"detail",detail+" Final 80x25 viewport; long displays scroll. Draft settings are not saved."}};
}
