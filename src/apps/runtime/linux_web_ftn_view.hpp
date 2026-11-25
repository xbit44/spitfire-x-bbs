#pragma once
#include "linux_web_ftn_drafts.hpp"
namespace sfx::apps::runtime::web {
std::string web_ftn_token_text(application::FtnTransferToken);
application::FtnTransferToken web_ftn_token(std::string_view);
nlohmann::json web_ftn_draft_summary(const WebFtnDraft&);
nlohmann::json web_ftn_draft_page(const WebFtnDraft&,const nlohmann::json&);
} // namespace sfx::apps::runtime::web
