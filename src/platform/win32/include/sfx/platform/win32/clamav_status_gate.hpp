#pragma once
#include "sfx/platform/win32/file_payload_store.hpp"
namespace sfx::platform::win32 {
// The status reader never changes definitions/history/settings. Retain the
// trusted private Work/ClamAV directory and this shared operation-file lease
// while hashing, so a status view cannot obstruct a later definition rename.
// No vendor child or scratch-byte reservation is needed for this short read.
installation::Handle clamav_status_gate(const FilePayloadDirectory& work,
    const std::wstring& owner,const std::function<bool()>& cancelled={},
    std::uint32_t wait_milliseconds=5000);
}
