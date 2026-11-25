#pragma once
#include "sfx/application/ftn_held_control.hpp"
#include <nlohmann/json.hpp>
namespace sfx::apps::runtime::web {
application::FtnHeldReviewQuery web_ftn_held_query(const nlohmann::json&);
application::FtnHeldRecipientQuery web_ftn_held_recipients_query(const nlohmann::json&);
application::FtnHeldControlRequest web_ftn_held_action(const nlohmann::json&);
nlohmann::json web_ftn_held_reply(const application::FtnHeldControlResponse&);
}
