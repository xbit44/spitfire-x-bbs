#pragma once
#include "sfx/application/local_account_administration.hpp"
#include <functional>
namespace sfx::apps::runtime {
class LinuxSecurityPublication final:public application::ISecurityPolicyPublication {
public:
    std::function<application::StorageStatus(const application::LocalSecuritySnapshot&)> apply;
    std::string fingerprint(std::string_view text)override;
    application::StorageStatus publish(const application::LocalSecuritySnapshot& truth)override{
        return apply?apply(truth):application::StorageStatus{application::StorageErrorCode::internal,"security_publication_unavailable"};
    }
};
}
