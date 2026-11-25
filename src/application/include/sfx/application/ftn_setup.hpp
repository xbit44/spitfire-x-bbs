#pragma once
#include "sfx/application/ftn.hpp"
#include <utility>

namespace sfx::application {

struct FtnEchoBulkOptions {
    std::optional<std::uint16_t> access, reading, posting;
    bool copy_count{}, copy_age{};
    std::optional<std::uint32_t> maximum_messages{500}, maximum_age_days;
};
struct FtnSetupResult {
    std::size_t changed{};
    std::vector<FtnValidationIssue> issues;
    bool succeeded() const noexcept { return issues.empty(); }
};

// These functions operate only on a caller-owned draft. They never save,
// allocate permanent message IDs, queue mail, contact a hub or expire content.
FtnSetupResult add_ftn_setup_network(FtnConfiguration&,FtnNetwork,
    const std::vector<std::pair<std::int64_t,std::string>>& existing_groups);
FtnSetupResult add_ftn_setup_echo(FtnConfiguration&,std::string_view network,
    std::string_view tag,std::string_view description={},std::string_view caller_name={});
FtnSetupResult add_ftn_setup_echoes(FtnConfiguration&,std::string_view network,
    const std::vector<FtnEchoListEntry>& selected_entries);
FtnSetupResult apply_ftn_echo_options(FtnConfiguration&,std::string_view network,
    const std::vector<std::string>& explicit_targets,const FtnEchoBulkOptions&);
struct FtnStoredDirectory {std::string directory;std::vector<std::string> networks;};
// One scan per configured directory; each source keeps an explicit eligible
// network set. Existing global settings remain in force only for legacy nets.
std::vector<FtnStoredDirectory> ftn_stored_directories(const FtnConfiguration&);

} // namespace sfx::application
