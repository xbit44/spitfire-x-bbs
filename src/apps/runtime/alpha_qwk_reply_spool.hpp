#pragma once
#include "sfx/application/qwk_reply_storage.hpp"
#include <filesystem>
#include <functional>

namespace sfx::apps::runtime {
// Only a host-created protected packet job and fully validated in-memory codec
// result enter this factory. Caller names/paths are never native file paths.
application::StorageResult<std::shared_ptr<application::IQwkReplySource>> seal_qwk_reply_spool(
    const std::filesystem::path& private_job,const std::wstring& installation_owner,
    const application::QwkReplyPacket&,std::uint64_t compressed_bytes,
    std::uint64_t expanded_bytes,std::function<bool()> cancelled={},
    std::shared_ptr<void> private_job_lifetime={},void* pinned_private_directory=nullptr,
    std::function<void()> confirmed_completion={});
} // namespace sfx::apps::runtime
