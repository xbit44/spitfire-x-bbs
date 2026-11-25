#pragma once
#include "sfx/application/local_account_administration.hpp"
#include "sfx/application/qwk_storage.hpp"
namespace sfx::application {
struct QwkPolicyChange {Identifier128 operation;std::int64_t expected_revision{};std::string board_id;std::uint32_t maximum_messages{};};
struct LocalQwkPolicyResult {LocalAccountResultCode code{LocalAccountResultCode::unavailable};QwkPolicy policy{};};
bool valid_qwk_policy_change(const QwkPolicyChange&)noexcept;
class LocalQwkAdministration final {
public:
    LocalQwkAdministration(IQwkStorage& s,LocalAccountAuthority a):storage_(s),authority_(a){}
    LocalQwkPolicyResult snapshot();
    LocalQwkPolicyResult execute(const QwkPolicyChange&);
private:
    IQwkStorage& storage_;LocalAccountAuthority authority_;
};
} // namespace sfx::application
