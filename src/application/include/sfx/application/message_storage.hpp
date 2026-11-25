#pragma once
#include "sfx/application/message.hpp"
#include "sfx/application/message_arrival.hpp"
#include <memory>

namespace sfx::application {
struct MessageCanvas;
struct MessageQueryCursor {
    bool history{};
    std::int64_t group_order{-1}, group_id{}, echo_order{-1}, echo_id{}, area_number{}, message_id{}, arrival{};
    bool reverse{};
};
enum class MessageListDeleteScope : std::uint8_t { none, inbox, everyone };
struct MessageHeader {
    CommittedMessage identity;
    MessageTarget target;
    std::optional<AccountId> author;
    struct RemoteAuthor {std::string network,address,name;};
    std::string from, to, subject;
    std::string group_name, echo_name;
    std::uint16_t conference_number{};
    std::optional<MessageId> parent;
    bool recipient_opened{}, inbox_removed{};
    std::int64_t recipient_opened_sequence{};
    MessageQueryCursor position;
    bool permanent{};
    std::optional<RemoteAuthor> remote_author;
    std::string remote_delivery;
    bool list_deleted{}, list_delete_allowed{};
    MessageListDeleteScope list_delete_scope{MessageListDeleteScope::none};
};
struct OpenedMessage {
    MessageHeader header;
    MessageBody body;
    std::optional<MessageSource> original_source;
    bool pipe_colors{};
    std::optional<MessageBody> plain_rendition_source;
    bool ansi_colors{true}; // Reading is independent of outgoing echo policy.
    std::shared_ptr<const MessageCanvas> received_canvas; // Derived v2, never original.
};
struct MessageCatalogCursor {
    std::int64_t configuration_revision{}, after_order{-1}, after_id{};
    std::size_t page_size{kMessagePageDefault};
};
template<class T> struct MessageCatalogPage {
    std::vector<T> entries;
    std::int64_t configuration_revision{};
    std::optional<MessageCatalogCursor> next;
};
enum class MessageQueryMode : std::uint8_t { echo, addressed, regular_new, personal, inbox, sent, correspondence };
enum class MessageQueryScope : std::uint8_t { current_echo, current_group, all_accessible };
enum class MessageCallerDirection : std::uint8_t { to, from, both };
struct MessageQueryOptions {
    MessageQueryMode mode{MessageQueryMode::echo};
    MessageQueryScope scope{MessageQueryScope::current_echo};
    LocalEchoId echo;
    MessageGroupId group{};
    std::optional<AccountId> addressed_to{};
    std::string search{};
    bool unread_only{}; // Login personal scan only; menu history is unchanged.
    MessageCallerDirection caller_direction{MessageCallerDirection::to};
    std::string remote_name{};
    std::string remote_network{};
    std::string remote_address{};
};
// A bounded logical snapshot: arrival fence prevents an endless scan;
// recipient-read fence prevents an opened unread item reappearing as history.
struct MessageQueryContext {
    MessageQueryOptions options;
    AccountId owner{};
    std::int64_t arrival_fence{}, recipient_read_fence{}, configuration_revision{};
    // At most 999 public echoes, never a materialized message-ID list. This
    // entry progress permits Previous without rewinding durable caller state.
    std::vector<std::pair<LocalEchoId,std::int64_t>> initial_progress{};
};
struct MessageGroupScanEntry {
    LocalEcho echo;
    MessageScanPreference preference;
};
struct MessageLoginScanSummary {
    MessageQueryContext query;
    std::int64_t scanned{}, unread_to_you{}, read_to_you{}, from_you{};
};
struct MessageGroupScanPreview {
    MessageGroupId group;
    std::int64_t configuration_revision{}, group_revision{};
    MessageScanColumn column{MessageScanColumn::unread};
    std::vector<MessageGroupScanEntry> entries;
    bool enable{}; // mixed -> On; all On -> Off; empty -> no-op
};
struct MessageHeaderPage {
    std::vector<MessageHeader> entries;
    std::optional<MessageQueryCursor> next;
};
struct MessageNetworkChoice { std::string code,name,source_aka;bool paused{}; };
// Trusted host authority, never decoded from a caller or Config payload.
struct MessageConfigurationActor {
    bool installation_owner{};
    std::optional<MessageActor> caller;
};
enum class MessageConfigurationAction : std::uint8_t { group, echo, clone_permissions, system_sysop, line_limit, delete_echo, insert_echo, move_echo_up, move_echo_down, delete_group, private_presentation, unread_mail_retention };
struct MessageEchoRevision { LocalEchoId echo; std::int64_t revision{}; };
struct MessageConfigurationChange {
    Identifier128 operation;
    MessageConfigurationAction action{MessageConfigurationAction::group};
    std::int64_t expected_configuration_revision{};
    MessageGroup group; // id zero creates; otherwise revision is expected
    LocalEcho echo; // id zero creates; sequence is never a configuration value
    LocalEchoId clone_source;
    std::int64_t source_revision{};
    std::vector<MessageEchoRevision> clone_targets;
    std::uint8_t permission_mask{}; // Access=1, Reading=2, Posting=4; initially zero
    AccountId sysop{};
    std::int64_t expected_assignment_revision{}, expected_account_revision{};
    std::uint16_t security_level{};
    std::uint32_t maximum_lines{};
    std::int64_t expected_limit_revision{};
    // Insert before this stable echo; never a visible row number.
    LocalEchoId insert_before;
    std::int64_t insert_before_revision{};
    std::uint8_t presentation_mask{}; // Echo pipe color=1; unrelated/legacy=0.
    std::int64_t expected_private_presentation_revision{};
    bool private_pipe_colors{};
    std::uint8_t retention_mask{}; // Count=1, Age=2; clone boxes initially zero.
    std::int64_t expected_unread_retention_revision{};
    std::optional<std::uint32_t> unread_mail_age_days{kDefaultUnreadMailAgeDays};
};
struct MessageConfigurationResult {
    std::int64_t configuration_revision{}, entity_id{}, entity_revision{};
    bool replayed{};
};
struct MessageLineLimit { std::uint16_t security_level{};std::uint32_t maximum_lines{};std::int64_t revision{}; };
bool valid_message_configuration_change(const MessageConfigurationChange&);
bool valid_message_catalog_cursor(const MessageCatalogCursor&);
struct MessageConfigurationSnapshot {
    std::int64_t revision{};
    std::vector<MessageGroup> groups;
    std::vector<LocalEcho> echoes;
    std::optional<AccountId> system_sysop;
    std::int64_t assignment_revision{};
    std::optional<MessageCatalogCursor> next;
    std::vector<MessageLineLimit> line_limits; // active authoritative levels, at most 999
    std::int64_t private_presentation_revision{};
    bool private_pipe_colors{};
    std::int64_t unread_retention_revision{1};
    std::optional<std::uint32_t> unread_mail_age_days{kDefaultUnreadMailAgeDays};
};
struct MessageCommitOutcome { std::optional<CommittedMessage> committed; };
struct MessageArea { MessageGroup group; LocalEcho echo; std::int64_t configuration_revision{}; };
struct MessageCallerStatistics {
    std::int64_t messages_entered{}, mail_total{}, mail_unread{}, last_arrival{};
    std::optional<std::int64_t> echo_count, new_messages;
};
struct MessageArrivalPage { std::vector<MessageArrival> arrivals;std::optional<std::int64_t> next_after;std::int64_t fence{}; };
struct MessageRecipient {
    AccountId account{};
    std::int64_t revision{};
    std::string name;
};
struct MessageRecipientPage {
    std::vector<MessageRecipient> entries;
    std::optional<MessageRecipient> next_after;
};
// Private storage boundary; its implementation owns all SQL and actor checks.
class IMessageStorage {
public:
    virtual ~IMessageStorage() = default;
    virtual StorageResult<std::vector<MessageNetworkChoice>> ftn_message_networks(const MessageActor&){return {{StorageErrorCode::not_found,"FTN message networks unavailable"},{}};}
    virtual StorageResult<MessageConfigurationSnapshot> message_configuration(const MessageConfigurationActor&,
        std::optional<MessageGroupId>, const MessageCatalogCursor&) = 0;
    virtual StorageResult<MessageConfigurationResult> change_message_configuration(const MessageConfigurationActor&, const MessageConfigurationChange&) = 0;
    virtual StorageResult<MessageRecipientPage> resolve_message_recipient(const MessageActor&, std::string_view,
        const std::optional<MessageRecipient>& after = {}, bool name_only=false) = 0;
    virtual StorageResult<MessageTarget> system_sysop_target(const MessageActor&) = 0;
    virtual StorageResult<std::optional<MessageArea>> saved_message_area(const MessageActor&){return {StorageStatus::success(),std::optional<MessageArea>{}};}
    virtual StorageStatus remember_message_area(const MessageActor&,LocalEchoId){return StorageStatus::success();}
    virtual StorageResult<MessageArea> message_area(const MessageActor&,LocalEchoId) = 0;
    virtual StorageResult<MessageCallerStatistics> message_caller_statistics(const MessageActor&,std::optional<LocalEchoId>) = 0;
    virtual StorageResult<MessageArrivalPage> message_arrival_page(const MessageActor&,std::int64_t after,std::size_t limit=kMessagePageDefault) = 0;
    virtual StorageResult<bool> message_arrival_eligible(const MessageActor&,MessageId) = 0;
    // Current recipient eligibility and sender metadata in one read snapshot; no Read mutation.
    virtual StorageResult<std::optional<std::string>> message_arrival_sender(const MessageActor&,MessageId) = 0;
    virtual StorageResult<bool> has_addressed_unread(const MessageActor&,bool personal_scan) = 0;
    virtual StorageResult<MessageLoginScanSummary> login_message_scan_summary(const MessageActor&,bool include_authored_echoes=false) = 0;
    virtual StorageResult<std::vector<MessageGroup>> message_groups(const MessageActor&) = 0;
    virtual StorageResult<std::vector<LocalEcho>> local_echoes(const MessageActor&, MessageGroupId) = 0;
    virtual StorageResult<MessageCatalogPage<MessageGroup>> message_group_page(const MessageActor&, const MessageCatalogCursor&) = 0;
    virtual StorageResult<MessageCatalogPage<LocalEcho>> local_echo_page(const MessageActor&, MessageGroupId, const MessageCatalogCursor&) = 0;
    virtual StorageResult<MessageQueryContext> begin_message_query(const MessageActor&, const MessageQueryOptions&) = 0;
    virtual StorageResult<MessageHeaderPage> query_message_headers(const MessageActor&, const MessageQueryContext&,
        const MessageQueryCursor&, std::size_t page_size = kMessagePageDefault) = 0;
    virtual StorageResult<MessageHeaderPage> query_message_list_headers(const MessageActor& caller,
        const MessageQueryContext& context, const MessageQueryCursor& cursor, std::size_t page_size = kMessagePageDefault) {
        return query_message_headers(caller,context,cursor,page_size);
    }
    virtual StorageStatus set_message_list_deleted(const MessageActor&, const MessageQueryContext&,
        MessageId, bool, bool, MessageListDeleteScope) {
        return {StorageErrorCode::permission_denied,"change message list deletion"};
    }
    virtual StorageResult<MessageHeader> message_number_header(const MessageActor&, const MessageQueryContext&, std::int64_t number) = 0;
    // Traversal opens recheck unread qualification inside the write transaction.
    // Manual list/Previous opens require current query scope/visibility instead.
    virtual StorageResult<OpenedMessage> open_query_message(const MessageActor&, const MessageQueryContext&,
        MessageId, bool traversal, bool history, bool preview=false) = 0;
    virtual StorageResult<ComposeContext> begin_message(const MessageActor&, const MessageTarget&,
        std::optional<MessageId> parent = {}) = 0;
    virtual StorageResult<CommittedMessage> commit_message(const CommitMessageRequest&) = 0;
    StorageResult<CommittedMessage> commit_source_message(const CommitSourceMessageRequest& r) {
        return commit_message({r.actor,r.operation,r.context,r.subject,{},r.source});
    }
    // Explicit operation reconciliation reuses the original canonical request.
    virtual StorageResult<CommittedMessage> reconcile_message(const CommitMessageRequest&) = 0;
    // Success with no receipt is confirmed not committed. Authority/storage
    // failure is never confused with the absence of a committed receipt.
    virtual StorageResult<MessageCommitOutcome> message_commit_outcome(const CommitMessageRequest&) = 0;
    virtual StorageResult<OpenedMessage> open_message(const MessageActor&, MessageId) = 0;
    virtual StorageResult<OpenedMessage> quote_source(const MessageActor&, MessageId) = 0;
    virtual StorageResult<MessageScanPreference> message_scan_preference(const MessageActor&, LocalEchoId) = 0;
    virtual StorageResult<MessageScanPreference> set_message_scan_preference(const MessageActor&,
        LocalEchoId, std::int64_t expected_revision, MessageScanColumn, bool enabled) = 0;
    virtual StorageResult<MessageGroupScanPreview> preview_group_scan_column(const MessageActor&, MessageGroupId, MessageScanColumn) = 0;
    virtual StorageResult<bool> toggle_group_scan_column(const MessageActor&, Identifier128, const MessageGroupScanPreview&) = 0;
    virtual StorageStatus remove_private_inbox_item(const MessageActor&, MessageId) = 0;
    // Live storage authority: level999 OR configured Sysop threshold. Host-only
    // actor/session binding is rechecked, never a caller-supplied privilege bit.
    virtual StorageResult<bool> message_delete_authority(const MessageActor&) = 0;
    // Content-free audited permanent deletion, stable target and replay token.
    // Same token/actor/target reconciles; changed target or lost privilege fails.
    virtual StorageResult<bool> delete_message(const MessageActor&, Identifier128, MessageId) = 0;
    virtual StorageResult<bool> message_permanent_state(const MessageActor&, MessageId) = 0;
    virtual StorageResult<bool> set_message_permanent(const MessageActor&, Identifier128,
        MessageId, bool expected_permanent, bool permanent) = 0;
};
} // namespace sfx::application
