#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace sfx::application {

enum class OutcomeKind : std::uint8_t { session, operation };

struct OutcomeRecord {
    std::string idempotency_key;
    OutcomeKind kind{OutcomeKind::session};
    std::string session_identity;
    std::string outcome_code;
    std::uint64_t elapsed_seconds{0};
};

class IOutcomeSink {
public:
    virtual ~IOutcomeSink() = default;
    virtual bool commit(const OutcomeRecord& record) = 0;
};

class IdempotentOutcomeFinalizer final {
public:
    explicit IdempotentOutcomeFinalizer(IOutcomeSink& sink);
    bool finalize(const OutcomeRecord& record);
    std::size_t finalized_count() const noexcept;

private:
    IOutcomeSink& sink_;
    std::unordered_set<std::string> finalized_keys_;
};

}  // namespace sfx::application

