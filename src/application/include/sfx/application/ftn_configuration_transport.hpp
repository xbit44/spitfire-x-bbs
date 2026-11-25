#pragma once
#include "sfx/application/ftn.hpp"
#include <array>
#include <functional>
#include <mutex>
#include <utility>

namespace sfx::application {
inline constexpr std::size_t kFtnConfigurationPageBytes=24*1024;
using FtnTransferToken=std::array<std::uint8_t,16>;
enum class FtnConfigurationOperation : std::uint8_t { read, begin, append, commit, cancel, reconcile, preview_retention };
enum class FtnConfigurationCode : std::uint8_t { ok, unavailable, invalid, conflict, busy };
struct FtnConfigurationRequest {
    FtnConfigurationOperation operation{FtnConfigurationOperation::read};
    FtnTransferToken token{};
    std::int64_t revision{};
    std::uint32_t offset{},total{};
    std::vector<std::uint8_t> bytes;
};
struct FtnConfigurationResponse {
    FtnConfigurationCode code{FtnConfigurationCode::unavailable};
    std::int64_t revision{};
    std::uint32_t offset{},total{};
    std::vector<std::uint8_t> bytes;
};
std::vector<std::uint8_t> encode_ftn_configuration_request(const FtnConfigurationRequest&);
std::optional<FtnConfigurationRequest> decode_ftn_configuration_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_ftn_configuration_response(const FtnConfigurationResponse&);
std::optional<FtnConfigurationResponse> decode_ftn_configuration_response(const std::vector<std::uint8_t>&);

// Called only behind the existing installation-owner authorization boundary.
// At most one pinned reader and one draft upload; TTL bounds abandoned state.
// No upload changes durable/live configuration until a validated CAS commit.
class FtnConfigurationPublisher final {
public:
    using Read=std::function<std::optional<FtnConfiguration>()>;
    using Change=std::function<FtnConfigurationResponse(const FtnTransferToken&,const FtnConfiguration&)>;
    using Reconcile=std::function<FtnConfigurationResponse(const FtnTransferToken&)>;
    FtnConfigurationPublisher(Read read,Change change,Reconcile reconcile={},Change preview={});
    FtnConfigurationResponse handle(const FtnConfigurationRequest&,std::uint64_t monotonic_millis);
private:
    struct Transfer {
        FtnTransferToken token{};
        std::int64_t revision{};
        std::uint32_t total{};
        std::uint64_t touched{};
        std::vector<std::uint8_t> bytes;
    };
    Read read_; Change change_; Reconcile reconcile_; Change preview_;
    std::mutex mutex_;
    std::optional<Transfer> reader_,writer_;
};
class FtnReadOnlyPublisher final {
public:
    using Snapshot=std::pair<std::int64_t,std::vector<std::uint8_t>>;
    explicit FtnReadOnlyPublisher(std::function<std::optional<Snapshot>()> read):read_(std::move(read)){}
    FtnConfigurationResponse handle(const FtnConfigurationRequest&,std::uint64_t monotonic_millis);
private:
    std::function<std::optional<Snapshot>()> read_;
    std::mutex mutex_;FtnTransferToken token_{};std::optional<Snapshot> snapshot_;std::uint64_t touched_{};
};
} // namespace sfx::application
