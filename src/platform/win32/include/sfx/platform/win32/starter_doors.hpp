#pragma once
#include "sfx/platform/win32/installation_security.hpp"

namespace sfx::platform::win32::installation {
struct StarterDoorFile {
    std::wstring relative_path;
    std::vector<std::uint8_t> bytes;
    std::string sha256;
};
void begin_fresh_starter_copy(const Path&,const std::wstring& owner,const std::string& installation_id);
// Fresh Setup only, before its protected installation receipt is committed.
void install_fresh_starter_doors(const Path&, const std::wstring& owner,
    const std::string& installation_id, const std::vector<StarterDoorFile>&);
void resume_owned_starter_doors(const Path& root,const Path& retained_source,
    const std::wstring& owner,const std::string& id,const std::vector<StarterDoorFile>&);
void commit_starter_door_copy(const Path&,const std::wstring& owner);
// Missing means ineligible. An invalid present component refuses completion.
bool verified_fresh_starter_doors(const Path&, const std::wstring& owner);
}
