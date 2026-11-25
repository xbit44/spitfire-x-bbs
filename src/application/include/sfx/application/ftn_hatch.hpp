#pragma once
#include "sfx/application/ftn_hatch_catalog.hpp"
#include "sfx/application/file_storage.hpp"
namespace sfx::application {
// Explicit owner choices only. Native payload identity and successful queueing
// are never accepted from IPC. A request names one immutable confirmation.
inline constexpr std::size_t kFtnHatchMaximumDestinations=1023;
struct FtnHatchRequest {
    Identifier128 operation;FileId file;
    std::int64_t file_revision{},configuration_revision{};
    std::string network,tag,origin,description;
    std::vector<std::string> destinations;
};
enum class FtnHatchState : std::uint8_t {preparing,queued,held};
struct FtnHatchRecord {
    FtnHatchRequest request;FilePayloadIdentity payload;
    std::string filename;FileAreaId source_area;FileOrigin source_origin{};
    std::optional<Identifier128> source_receipt;
    std::optional<FileObjectIdentity> preparer; // exact retained native exclusion object
    std::int64_t created{};std::uint32_t crc{};FtnHatchState state{FtnHatchState::preparing};
    std::string reason;
};
bool valid_ftn_hatch_request(const FtnHatchRequest&);
std::vector<std::uint8_t> encode_ftn_hatch_request(const FtnHatchRequest&);
std::optional<FtnHatchRequest> decode_ftn_hatch_request(const std::vector<std::uint8_t>&);
}
