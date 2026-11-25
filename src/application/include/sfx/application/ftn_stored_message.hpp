#pragma once
#include "sfx/application/ftn.hpp"
namespace sfx::application {
struct FtnStoredMessage {
    FtnAddress origin,destination;
    std::uint16_t attributes{};
    std::string from,to,subject;
    std::vector<std::string> attachments; // CP437 file specifications
    bool kill_files{},truncate_files{},hold{};
};
// FTS-0001 190-byte stored header plus NUL-terminated text. INTL supplies
// missing zones; FMPT/TOPT supply points. Never infer a missing zone.
std::optional<FtnStoredMessage> parse_ftn_stored_message(std::string_view);
}
