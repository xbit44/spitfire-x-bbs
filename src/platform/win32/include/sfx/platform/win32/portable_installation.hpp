#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <optional>
namespace sfx::platform::win32::installation {
// Copied-board input, not current-host Runtime/service authority. Historical
// root/owner/physical IDs describe the source only. Preparation revalidates the
// current files/consented operation before publishing new host-local authority.
Receipt read_portable_installation_input(const Path& current_root);
void write_portable_installation_input(const Receipt&);
// Diagnostic/preparation entry validates only its actual current image. An
// absent unrelated peer can be reported without making diagnosis unreachable.
Receipt verify_portable_entry(const Path& current_root,std::size_t program_role,DWORD process_id=0);
struct PreparedFirstStart {std::wstring host_id,operation;};
std::optional<PreparedFirstStart> read_prepared_first_start(const Path&,const std::wstring& current_owner);
void require_prepared_first_start(const Receipt&,const std::wstring& operation);
void acknowledge_prepared_first_start(const Path&,const std::wstring& current_owner,const std::wstring& operation={});
}
