#pragma once
#include <atomic>
#include <string>
#include <string_view>
namespace sfx::platform::win32 {
enum class FtnEchoListDownloadCode { ok, invalid_url, cancelled, too_large, timeout, certificate, redirect, http_error, transport };
struct FtnEchoListDownload {FtnEchoListDownloadCode code{FtnEchoListDownloadCode::transport};std::string bytes;};
// Owner-requested list data only; no scripts, peer connection or subscriptions.
// Optional trust input is for isolated native HTTPS qualification, never exposed
// as a certificate-disable UI setting. Production callers use embedded trust.
FtnEchoListDownload download_ftn_echo_list(std::string url,const std::atomic<bool>& cancel,const std::string* fixture_trust=nullptr);
std::string_view ftn_echo_list_download_message(FtnEchoListDownloadCode) noexcept;
}
