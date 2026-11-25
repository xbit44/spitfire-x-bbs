#include "sfx/application/accounting.hpp"

namespace sfx::application {

IdempotentOutcomeFinalizer::IdempotentOutcomeFinalizer(IOutcomeSink& sink) : sink_(sink) {}

bool IdempotentOutcomeFinalizer::finalize(const OutcomeRecord& record) {
    if (record.idempotency_key.empty() || finalized_keys_.count(record.idempotency_key) != 0) {
        return false;
    }
    if (!sink_.commit(record)) {
        return false;
    }
    finalized_keys_.insert(record.idempotency_key);
    return true;
}

std::size_t IdempotentOutcomeFinalizer::finalized_count() const noexcept { return finalized_keys_.size(); }

}  // namespace sfx::application

