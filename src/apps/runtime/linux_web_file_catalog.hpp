#pragma once
#include "sfx/application/local_file_administration.hpp"
#include <nlohmann/json.hpp>
namespace sfx::apps::runtime::web {
application::LocalFileConfigurationQuery file_catalog_query(const nlohmann::json&);
nlohmann::json file_catalog_page(const application::LocalFileConfigurationSnapshot&,const application::LocalFileConfigurationQuery&,std::string_view operation);
application::FileConfigurationChange file_catalog_update(const nlohmann::json&);
}
