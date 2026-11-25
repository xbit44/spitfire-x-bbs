    api_get("/api/control/about",[&](const auto& request,auto& response){
        if(!request.params.empty())throw std::runtime_error("about_query_invalid");
        json_response(response,{{"product",SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION},
            {"release_date",SFX_PRODUCT_RELEASE_DATE},{"platform","Linux AMD64"}});
    });
    api_get(R"(/help/sysop/(.*))",[&](const auto& request,auto& response){
        const auto asset=rt::web::read_help_asset(root,request.matches[1].str());
        if(!asset){response.status=404;response.set_content("SFX Wiki file unavailable. Restore docs/sysop from the matching SFX package, or check its ownership and permissions.","text/plain; charset=utf-8");return;}
        response.set_content(std::string(asset->bytes.begin(),asset->bytes.end()),asset->content_type);
    });
