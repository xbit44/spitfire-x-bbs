#pragma once

#include "sfx/application/ansi_playback_settings.hpp"
#include "sfx/application/display.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace sfx::platform::win32 {

application::AnsiPlaybackSettings::PathRules ansi_playback_path_rules() noexcept;

enum class TrustedDisplayReadStage : std::uint8_t {
    root_revalidated,
    candidate_opened,
    file_verified
};

class ITrustedDisplayReadHook {
public:
    virtual ~ITrustedDisplayReadHook() = default;
    virtual bool widen_candidate_sharing_for_race_test() const noexcept {
        return false;
    }
    virtual void on_stage(
        TrustedDisplayReadStage stage,
        std::wstring_view trusted_root,
        std::wstring_view opened_path) = 0;
};

class Win32TrustedDisplayAssetReader final
    : public application::ITrustedDisplayAssetReader {
public:
    static std::unique_ptr<Win32TrustedDisplayAssetReader> open(
        std::wstring trusted_root,
        ITrustedDisplayReadHook* test_hook = nullptr);

    Win32TrustedDisplayAssetReader(
        const Win32TrustedDisplayAssetReader&) = delete;
    Win32TrustedDisplayAssetReader& operator=(
        const Win32TrustedDisplayAssetReader&) = delete;
    ~Win32TrustedDisplayAssetReader() override;

    application::TrustedDisplayAssetReadResult read_asset(
        std::string_view logical_identity,
        std::size_t maximum_bytes) override;

private:
    Win32TrustedDisplayAssetReader(
        std::wstring canonical_root,
        std::wstring final_root,
        void* root_handle,
        std::uint64_t root_identity_high,
        std::uint64_t root_identity_low,
        ITrustedDisplayReadHook* test_hook) noexcept;

    std::wstring canonical_root_;
    std::wstring final_root_;
    void* root_handle_{nullptr};
    std::uint64_t root_identity_high_{0};
    std::uint64_t root_identity_low_{0};
    ITrustedDisplayReadHook* test_hook_{nullptr};
    std::atomic<bool> invalidated_{false};
};

}  // namespace sfx::platform::win32
