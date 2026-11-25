#pragma once
#include "sfx/application/door_storage.hpp"
#include "sfx/application/door_bulletin_preview.hpp"
#include <functional>
#include <nlohmann/json.hpp>
namespace sfx::apps::runtime::web {
using DoorSecretProtector=std::function<std::optional<std::vector<std::uint8_t>>(std::string_view)>;
nlohmann::json door_catalog_page(const application::DoorCatalog&,unsigned menu,std::string_view operation);
nlohmann::json door_editor_page(const application::DoorCatalog&,unsigned door,std::string_view operation);
application::DoorConfigurationChange door_catalog_update(nlohmann::json&,const application::DoorCatalog&,const DoorSecretProtector&);
// Exact current consequences, including shared placements, for the in-page review.
nlohmann::json door_removal_preview(const application::DoorCatalog&,std::string_view action,unsigned identity,unsigned number=0);
nlohmann::json door_bulletin_catalog_page(const application::DoorBulletinCatalog&,const application::DoorCatalog&,std::string_view operation);
application::DoorBulletinChange door_bulletin_update(const nlohmann::json&);
nlohmann::json door_bulletin_preview_page(const application::DoorBulletinPreview&);
}
