#pragma once
#include "sfx/application/local_message_administration.hpp"
#include <nlohmann/json.hpp>
namespace sfx::apps::runtime::web {
application::LocalMessageConfigurationQuery message_catalog_query(const nlohmann::json&);
nlohmann::json message_catalog_page(const application::LocalMessageConfigurationSnapshot&,const application::LocalMessageConfigurationQuery&,std::string_view operation);
application::MessageConfigurationChange message_catalog_update(const nlohmann::json&);
}
