#pragma once
#include "sfx/application/ftn.hpp"
#include <nlohmann/json.hpp>
#include <functional>

namespace sfx::apps::runtime::web {
using FtnSeal=std::function<std::optional<std::vector<std::uint8_t>>(std::string_view network,std::string_view link,unsigned kind,std::string_view secret)>;
bool web_ftn_same_id(std::string_view,std::string_view) noexcept;
nlohmann::json web_ftn_clone_preview(const nlohmann::json&,const application::FtnConfiguration&);
nlohmann::json web_ftn_settings(const application::FtnSettings&);
nlohmann::json web_ftn_network(const application::FtnNetwork&);
nlohmann::json web_ftn_link(const application::FtnLink&);
nlohmann::json web_ftn_echo(const application::FtnMessageEcho&);
nlohmann::json web_ftn_file(const application::FtnFileEcho&);
nlohmann::json web_ftn_route(const application::FtnRoute&);
nlohmann::json web_ftn_network_list(const application::FtnConfiguration&);
// The supplied configuration is an unpublished draft. Durable publication and
// exact processed-retention review use the separate FTN transfer adapter.
application::FtnConfiguration web_ftn_record_update(const nlohmann::json&,const application::FtnConfiguration&,const FtnSeal&);
} // namespace sfx::apps::runtime::web
