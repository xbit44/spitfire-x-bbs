#pragma once
#include "clamav_download.hpp"
namespace sfx::apps::setup {
// Setup owns protected installation. This ordinary coordinator invokes only the
// receipt-bound installed maintenance worker through normal Windows consent.
bool install_portable_clamav(const std::filesystem::path&,HWND,
    const ClamAvDownloadProgress&,const ClamAvDownloadPump&);
int clamav_maintenance_worker(const std::wstring& pipe,DWORD parent)noexcept;
} // namespace sfx::apps::setup
