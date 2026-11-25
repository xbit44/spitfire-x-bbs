#pragma once
#include "sfx/application/calling_time.hpp"
#include "sfx/application/chat.hpp"
#include "sfx/application/time.hpp"
#include <algorithm>
#include <stdexcept>

namespace sfx::apps::runtime {
// Shared Windows/Linux caller accounting composition. Elapsed time comes from
// the monotonic clock; local calendar labels come from the selected BBS zone.
// Operator-initiated Chat pauses normal calling time. Private caller Chat is
// independently charged and persisted before a successful snapshot is returned.
class CallerRuntimeAccounting final {
public:
    using PersistPrivate=std::function<application::StorageStatus(application::Identifier128,
        std::int64_t,const std::vector<std::pair<std::string,std::int64_t>>&)>;
    CallerRuntimeAccounting(std::uint64_t started,application::CallerLocalFormatter formatter,
        application::IClock& clock,application::ChatHub& chat,PersistPrivate persist)
        :started_(started),clock_(clock),chat_(chat),persist_(std::move(persist)),
         calling_(started,formatter),private_chat_(0,std::move(formatter)) {
        if(!persist_)throw std::invalid_argument("private_chat_accounting_port_missing");
    }
    application::SessionAccountingSnapshot snapshot(application::ChatIdentity published,bool timed_out) {
        const auto now=clock_.monotonic_millis();
        chat_.tick(now);
        if(published.session.valid()) {
            const auto paused=chat_.operator_paused_millis(published,now);
            if(paused)paused_=std::max(paused_,*paused);
            const auto usage=chat_.private_usage_millis(published,now);
            if(usage) {
                if(!private_chat_.advance(*usage,clock_.unix_seconds()))
                    throw std::runtime_error("private Chat accounting unavailable");
                if(private_chat_.total_seconds()!=private_persisted_) {
                    if(!persist_(published.session,private_chat_.total_seconds(),private_chat_.daily_seconds()))
                        throw std::runtime_error("private Chat accounting could not be saved");
                    private_persisted_=private_chat_.total_seconds();
                }
            }
        }
        if(now<started_||paused_>now-started_||!calling_.advance(now-paused_,clock_.unix_seconds()))
            throw std::runtime_error("caller accounting unavailable");
        return {calling_.total_seconds(),calling_.daily_seconds(),timed_out?
            std::optional<application::SessionOutcomeCode>{application::SessionOutcomeCode::timed_out}:std::nullopt};
    }
private:
    const std::uint64_t started_;
    application::IClock& clock_;
    application::ChatHub& chat_;
    PersistPrivate persist_;
    application::CallerTimeAccounting calling_,private_chat_;
    std::uint64_t paused_{};
    std::int64_t private_persisted_{};
};
} // namespace sfx::apps::runtime
