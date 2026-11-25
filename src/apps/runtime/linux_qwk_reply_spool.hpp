#pragma once
#include "sfx/application/qwk_reply_storage.hpp"
#include <filesystem>
#include <functional>

namespace sfx::apps::runtime {
// The host owns this private job. Packet names never become filesystem paths.
application::StorageResult<std::shared_ptr<application::IQwkReplySource>> seal_linux_qwk_reply_spool(
    const std::filesystem::path& private_job,const application::QwkReplyPacket&,
    std::uint64_t compressed_bytes,std::uint64_t expanded_bytes,
    std::function<bool()> cancelled={},std::shared_ptr<void> private_job_lifetime={},
    int pinned_private_directory=-1,std::function<void()> confirmed_completion={});
}
