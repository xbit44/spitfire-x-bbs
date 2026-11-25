#pragma once
#include "sfx/application/ftn.hpp"
#include <utility>
namespace sfx::application {
struct FtnEchoImportDestination {
    std::int64_t group{};
    std::string name;
    std::vector<FtnValidationIssue> issues;
};
// Resolve the current network's caller-facing group without writing or moving
// existing mappings. A zero result means create the named group at final Save.
FtnEchoImportDestination resolve_ftn_echo_import_destination(
    const FtnConfiguration&,std::string_view network,
    const std::vector<std::pair<std::int64_t,std::string>>& groups,
    const std::vector<std::pair<std::int64_t,std::int64_t>>& local_echo_groups);
struct FtnEchoImportRequirements {std::uint16_t access{10},read{10},post{10};};
// Seed only newly created echoes on a known hub. Saved exceptions and other
// links/networks remain unchanged; remote AreaFix management stays off.
void link_new_ftn_echoes_to_hub(const FtnConfiguration& before,FtnConfiguration& after,std::string_view network);
struct FtnEchoImportPlan {
    FtnConfiguration configuration;
    std::size_t selected{},created{},unchanged{},text_updates{};
    std::vector<FtnValidationIssue> issues;
};
// Pure preview only. The owner writer rechecks capacity/catalogs and commits
// selected mappings atomically. A list never deletes an absent existing echo.
FtnEchoImportPlan plan_ftn_echo_import(const FtnConfiguration&,std::string_view network,
    const FtnEchoList&,const std::vector<FtnEchoImportRequirements>&,
    std::size_t existing_local_count,std::int64_t group,std::string new_group,
    bool update_selected_text);
}
