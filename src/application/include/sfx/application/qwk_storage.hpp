#pragma once
#include "sfx/application/message_storage.hpp"
#include "sfx/application/qwk.hpp"
#include <functional>

namespace sfx::application {
inline constexpr std::size_t kQwkReferenceBatchMaximum=32;
inline constexpr std::size_t kQwkSelectionQueuesMaximum=8;
struct QwkPolicy {
    std::string board_id;
    std::uint32_t maximum_messages{kQwkDefaultMessageCount};
    std::int64_t revision{1};
    std::optional<bool> word_wrap_default; // durable policy; owner selected Off for shipping
    bool replayed{};
};
struct QwkSavedPreferences {QwkPreferences preferences;std::int64_t revision{1};};
struct QwkQueuePosition {
    LocalEchoId echo; // zero is the separate private received-mail queue
    std::int64_t preference_revision{1},initial_highest_opened{},after_arrival{};
    bool unread{true},to_you{true};
};
struct QwkAccessibleEcho {LocalEchoId echo;std::uint16_t conference{};std::string group_name,echo_name;};
struct QwkPacketPlan {
    AccountId owner{};
    Identifier128 operation;
    QwkPolicy policy;
    QwkSavedPreferences settings;
    std::int64_t arrival_fence{},catalog_revision{},continuation_revision{};
    std::uint16_t next_queue{};
    std::vector<QwkQueuePosition> queues; // bounded by mail + 999 public echoes
    std::vector<QwkAccessibleEcho> catalog; // currently accessible public metadata only
    std::array<std::uint8_t,32> selection_fingerprint{};
    std::optional<std::uint16_t> netmail_conference;
};
struct QwkDeliveredItem {MessageId message;LocalEchoId echo;std::int64_t arrival{},area_number{};};
struct QwkMessageCandidate {MessageId message;std::int64_t arrival{};};
template<class T> struct QwkQueueSelection {
    std::optional<T> selected;
    std::size_t queue{},scanned{}; // last examined queue, including a selected item
};
struct QwkDeliveryCommit {
    QwkPacketPlan initial;
    std::vector<QwkDeliveredItem> delivered;
    std::vector<QwkQueuePosition> final_queues;
    std::uint16_t next_queue{};
    bool more{};
    std::array<std::uint8_t,32> zip_sha256{};
    std::uint64_t compressed_bytes{},expanded_bytes{};
    std::function<bool()> cancelled; // trusted host lifetime/deadline, never fingerprinted or serialized
};
struct QwkDeliveryOutcome {std::uint32_t delivered{};bool more{},replayed{};};
struct QwkImportOutcome {std::uint32_t messages{},commands{},missing_parents{};bool replayed{};};
enum class QwkPointerMode : std::uint8_t { all_available, since_entry_time, caught_up };
struct QwkPointerState {LocalEchoId echo;std::int64_t preference_revision{},highest_opened{},captured_highest{},desired_highest{};};
struct QwkPointerChange {
    Identifier128 operation;
    MessageQueryScope scope{MessageQueryScope::current_echo};
    LocalEchoId echo;
    MessageGroupId group;
    QwkPointerMode mode{QwkPointerMode::all_available};
    std::optional<std::int64_t> since_utc;
    std::int64_t expected_catalog_revision{};
    std::vector<QwkPointerState> expected; // captured accessible targets; at most999
};
// Dedicated optional port: prior IMessageStorage consumers/fakes do not gain
// offline authority or new pure-virtual requirements. No caller deserializes
// a plan, file hash, transfer-success flag or trusted native path into this API.
class IQwkStorage {
public:
    virtual ~IQwkStorage()=default;
    virtual StorageResult<QwkPolicy> qwk_policy(const MessageConfigurationActor&)=0;
    virtual StorageResult<QwkPolicy> save_qwk_policy(const MessageConfigurationActor&,Identifier128,
        std::int64_t expected_revision,std::string_view board_id,std::uint32_t maximum_messages)=0;
    virtual StorageResult<QwkSavedPreferences> qwk_preferences(const MessageActor&)=0;
    virtual StorageResult<QwkSavedPreferences> save_qwk_preferences(const MessageActor&,std::int64_t expected_revision,const QwkPreferences&)=0;
    virtual StorageResult<QwkPacketPlan> begin_qwk_packet(const MessageActor&,Identifier128)=0;
    // Read-only live selection/permission check during a prepared download.
    // No body loading, new plan/reference allocation, receipt or progress write.
    virtual StorageResult<bool> validate_qwk_packet_authority(const MessageActor&,const QwkPacketPlan&)=0;
    virtual StorageResult<std::optional<OpenedMessage>> next_qwk_message(const MessageActor&,const QwkPacketPlan&,std::size_t queue)=0;
    // Prefetch owns only identity/cursor metadata. The builder still repeats
    // live selection and a complete authorized read immediately before encoding.
    virtual StorageResult<std::optional<QwkMessageCandidate>> next_qwk_candidate(
        const MessageActor& a,const QwkPacketPlan& p,std::size_t queue){
        const auto next=next_qwk_message(a,p,queue);
        if(!next||!next.value)return {next.status,{}};
        if(!next.value->has_value())return {StorageStatus::success(),std::optional<QwkMessageCandidate>{}};
        const auto& identity=next.value->value().header.identity;
        return {StorageStatus::success(),std::optional<QwkMessageCandidate>{{identity.id,identity.arrival_ordinal}}};
    }
    // A bounded logical round-robin read. Production validates the complete
    // plan once in a fresh snapshot, retains no authority, and loads at most
    // one body. The host checks cancellation/deadlines between these calls.
    virtual StorageResult<QwkQueueSelection<QwkMessageCandidate>> select_qwk_candidate(
        const MessageActor& a,const QwkPacketPlan& p,std::size_t queue,std::size_t maximum){
        if(queue>=p.queues.size()||maximum==0||maximum>kQwkSelectionQueuesMaximum||maximum>p.queues.size())return {{StorageErrorCode::invalid_argument,"validate bounded QWK selection"},{}};
        QwkQueueSelection<QwkMessageCandidate> result;
        for(std::size_t n=0;n<maximum;++n){result.queue=(queue+n)%p.queues.size();++result.scanned;auto next=next_qwk_candidate(a,p,result.queue);if(!next||!next.value)return {next.status,{}};if(next.value->has_value()){result.selected=std::move(**next.value);break;}}
        return {StorageStatus::success(),std::move(result)};
    }
    virtual StorageResult<QwkQueueSelection<OpenedMessage>> select_qwk_message(
        const MessageActor& a,const QwkPacketPlan& p,std::size_t queue,std::size_t maximum){
        if(queue>=p.queues.size()||maximum==0||maximum>kQwkSelectionQueuesMaximum||maximum>p.queues.size())return {{StorageErrorCode::invalid_argument,"validate bounded QWK selection"},{}};
        QwkQueueSelection<OpenedMessage> result;
        for(std::size_t n=0;n<maximum;++n){result.queue=(queue+n)%p.queues.size();++result.scanned;auto next=next_qwk_message(a,p,result.queue);if(!next||!next.value)return {next.status,{}};if(next.value->has_value()){result.selected=std::move(**next.value);break;}}
        return {StorageStatus::success(),std::move(result)};
    }
    virtual StorageResult<std::uint32_t> qwk_reference(const MessageActor&,MessageId)=0;
    // Optional bounded metadata-only batch. Adapters retain current authority,
    // immutable creation identity and never-reused references for every item.
    virtual StorageResult<std::vector<std::uint32_t>> qwk_references(const MessageActor& a,const std::vector<MessageId>& ids){
        if(ids.empty()||ids.size()>kQwkReferenceBatchMaximum)return {{StorageErrorCode::invalid_argument,"validate QWK reference batch"},{}};
        std::vector<std::uint32_t> refs;refs.reserve(ids.size());
        for(const auto id:ids){auto ref=qwk_reference(a,id);if(!ref||!ref.value)return {ref.status,{}};refs.push_back(*ref.value);}
        return {StorageStatus::success(),std::move(refs)};
    }
    // Runtime calls only after exact, actual wire completion. Same request can
    // reconcile a commit uncertainty; state and receipt are one writer commit.
    virtual StorageResult<QwkDeliveryOutcome> commit_qwk_delivery(const MessageActor&,const QwkDeliveryCommit&)=0;
    virtual StorageResult<std::optional<QwkDeliveryOutcome>> qwk_delivery_outcome(const MessageActor&,const QwkDeliveryCommit&)=0;
    virtual StorageResult<QwkPointerChange> preview_qwk_public_pointers(const MessageActor&,const QwkPointerChange&)=0;
    virtual StorageStatus change_qwk_public_pointers(const MessageActor&,const QwkPointerChange&)=0;
};
} // namespace sfx::application
