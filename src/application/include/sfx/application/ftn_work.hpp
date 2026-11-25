#pragma once
#include "sfx/application/ftn_mail.hpp"
#include "sfx/application/message_storage.hpp"
#include "sfx/application/file_storage.hpp"
#include "sfx/application/ftn_hatch.hpp"
#include "sfx/application/ftn_retention.hpp"
namespace sfx::application {
using FtnDigest=std::array<std::uint8_t,32>;
struct FtnSerialState {std::uint32_t high_water{};bool recovery_required{};Identifier128 lineage{};bool predecessor{};std::int64_t recovery_generation{};};
struct FtnSerialRecoveryRequest {Identifier128 operation;FtnSerialState expected;std::optional<FtnSerialState> prior;};
struct FtnSerialRecoveryReceipt {
    Identifier128 operation,lineage;
    std::int64_t generation{},utc{};
    std::uint32_t old_floor{},new_floor{};
    bool prior_known{};
    std::uint64_t held_outbound{},held_ingress{};
};
struct FtnHistoryRecoveryResult {
    StorageStatus status;
    std::optional<FtnSerialRecoveryReceipt> receipt;
    bool report_written{};
};
struct FtnFileProgress {std::string name;std::uint64_t size{},position{};bool active{},awaiting_ack{};};
struct FtnQueueRow {Identifier128 id;bool outbound{};std::string network,link,file,reason;std::uint8_t state{};std::uint64_t bytes{};};
struct FtnQueuePage {
    std::array<std::uint64_t,5> ingress{};std::array<std::uint64_t,6> outbound{};
    std::uint64_t unscanned{};bool more{};std::vector<FtnQueueRow> rows;
};
enum class FtnIngressProof : std::uint8_t { nonsecure,inbound_cram,inbound_legacy,outbound_verified_tls,outbound_tcp,external_packet,inbound_tcp };
inline bool ftn_verified_peer(FtnIngressProof proof,bool outbound)noexcept{
    switch(proof){case FtnIngressProof::inbound_cram:case FtnIngressProof::inbound_legacy:return !outbound;case FtnIngressProof::outbound_verified_tls:return outbound;default:return false;}
}
inline bool ftn_queue_offer_allowed(FtnIngressProof proof,bool outbound)noexcept{
    if(outbound)return proof==FtnIngressProof::outbound_verified_tls||proof==FtnIngressProof::outbound_tcp;
    return proof==FtnIngressProof::inbound_cram||proof==FtnIngressProof::inbound_legacy;
}
enum class FtnIngressState : std::uint8_t { partial,received,processing,held,processed };
struct FtnIngressReceipt {
    Identifier128 id,session;FtnDigest identity{},digest{};
    std::string network,link;FtnIngressProof proof{FtnIngressProof::nonsecure};bool outbound{};
    std::int64_t configuration_revision{};BinkpFile file;
    std::uint64_t received{};FtnIngressState state{FtnIngressState::partial};std::string reason;
    // Trusted local completed-receive observation. Separate from Native proof:
    // the external hook does not identify CRAM/legacy or session direction.
    bool external_session_authenticated{};
};
inline bool ftn_verified_receipt(const FtnIngressReceipt& r)noexcept{
    return ftn_verified_peer(r.proof,r.outbound)||
        (r.proof==FtnIngressProof::external_packet&&!r.outbound&&r.external_session_authenticated);
}
// Secret-free native parsing observation. Never constructed from owner IPC.
// Full packet bodies/passwords remain in restricted native custody.
struct FtnHeldMessage {
    Identifier128 operation,ingress;std::uint16_t message_index{};
    std::int64_t configuration_revision{};FtnDigest identity{},content{};
    FilePayloadIdentity payload;
    std::string network,envelope_destination,origin,destination,from,to,echo,reason;
    bool local_private{};
};
struct FtnRecipientResolutionRequest {
    Identifier128 operation,held_message,previous_resolution;
    std::int64_t configuration_revision{};
    AccountId recipient{};std::int64_t recipient_revision{};
};
struct FtnRecipientResolution {FtnRecipientResolutionRequest request;std::int64_t created{};};
struct FtnReprocessRequest {
    Identifier128 operation,ingress,previous_request;
    std::int64_t configuration_revision{};
};
struct FtnReprocessRecord {
    FtnReprocessRequest request;std::int64_t created{};
    bool finished{},processed{};std::string reason;
};
struct FtnTicRetryRequest {
    Identifier128 operation,ingress,previous_request;
    std::int64_t configuration_revision{};FtnDigest review{};
};
struct FtnTicRetryRecord {
    FtnTicRetryRequest request;std::int64_t created{};
    bool finished{},processed{};std::string reason;
};
// Redacted observation and opaque review token only. No native capability,
// credential, authentication assertion or mutable publication intent on IPC.
struct FtnHeldTicDetail {
    FtnDigest review{};Identifier128 publication;
    std::string claimed_from,echo,payload;
    std::uint8_t phase{};bool forward_plan_present{};
    std::optional<FtnTicRetryRecord> retry;
};
// Purpose-limited owner review projection. No body, subject, credential,
// native path, native identity or authentication assertion is accepted from UI.
struct FtnHeldReviewQuery {
    Identifier128 ingress,after;std::uint32_t message_offset{};
    std::string network,reason,find;
};
struct FtnHeldReviewRow {
    Identifier128 ingress;std::int64_t received_utc{};
    std::string network,link,file,reason,from;
    FtnIngressState state{FtnIngressState::held};bool retry_pending{},tic{};
};
struct FtnHeldReviewMessage {
    Identifier128 operation;std::uint16_t message_index{};
    std::string network,from,to,origin,destination,echo,reason;
    bool local_private{},committed{};
    std::optional<FtnRecipientResolution> resolution;
};
struct FtnHeldReview {
    std::int64_t configuration_revision{};bool more{},serial_recovery_required{};
    FtnMailerMode mode{FtnMailerMode::disabled};
    std::vector<FtnHeldReviewRow> rows;
    std::vector<FtnHeldReviewMessage> messages;
    std::optional<FtnReprocessRecord> retry;
    std::optional<FtnHeldTicDetail> tic;
};
struct FtnHeldRecipientQuery {AccountId after{};std::string find;};
struct FtnHeldRecipient {AccountId id{};std::int64_t revision{};std::string name;};
struct FtnHeldRecipients {std::vector<FtnHeldRecipient> rows;bool more{};};
enum class FtnOutboundKind : std::uint8_t { packet,tic,payload };
enum class FtnOutboundState : std::uint8_t { queued,claimed,awaiting_ack,delivered,held,external_handoff };
struct FtnOutboundJob {
    Identifier128 id,operation,claim,generation;std::uint16_t ordinal{};
    std::string network,link;FtnOutboundKind kind{FtnOutboundKind::packet};
    FtnMailFlavor flavor{FtnMailFlavor::normal};BinkpFile file;FtnDigest digest{};
    FtnOutboundState state{FtnOutboundState::queued};std::uint32_t attempts{};std::string reason;
    std::string echo_tag; // Empty for NetMail/control/files; used for Echo-only pause.
};
struct FtnSpoolObject {
    FtnOutboundJob job;std::int64_t configuration_revision{};
    std::optional<FileObjectIdentity> object;bool ready{};
};
// Stopped-owner review includes pending jobs even on disabled links.
// Held/completed records are preserved and do not require live payloads.
struct FtnHistoryRecoveryQueue {
    std::vector<FtnOutboundJob> outbound;
    std::vector<FtnIngressReceipt> ingress;
};
struct FtnExternalPublication {
    FtnOutboundJob job;
    std::string busy_path,flow_path,copy_path;
    std::uint8_t state{}; // 0: prepared, 1: handed off, 2: uncertain/held
};
struct FtnImportRequest {
    Identifier128 operation,ingress;std::uint16_t message_index{};
    std::int64_t expected_configuration_revision{};
    std::string network;
    // Envelope addressing retained; password MUST be erased after the worker's
    // separate credential check, before submitting work to the writer queue.
    FtnPacketHeader header;bool packet_password_verified{};
    FtnDigest identity{},content{};FtnPackedMessage message;
    // Verified immutable artifacts already in restricted custody. Commit them
    // atomically with local import or pass-through receipt, never remote ACK.
    std::vector<FtnOutboundJob> forwarding;
};
struct FtnImportResult { std::optional<CommittedMessage> message;bool replayed{}; };
struct FtnTossProgress {std::uint16_t message_index{};FtnDigest identity{},content{};};
struct FtnScanIntent {
    MessageId message;Identifier128 operation;
    std::string network,source_aka,tag,via_link,msgid,reply,destination,from,to,subject,body,reason;
    std::int64_t created{};FtnDigest fingerprint{};std::string origin_line;
};
// Secret-free result of native packet parsing and separate credential checks.
// Only the installation-owned FTN worker can construct this writer request.
struct FtnAreaFixRequest {
    Identifier128 operation,ingress;std::uint16_t message_index{};
    std::int64_t expected_configuration_revision{};std::string network,requesting_link;
    FtnAddress origin,destination,envelope_origin,envelope_destination;
    std::string from,reply;FtnDigest identity{},content{};
    std::vector<FtnAreaFixCommand> commands;
    bool password_verified{},packet_password_verified{};
};
struct FtnAreaFixResult {bool replayed{},configuration_changed{};std::int64_t revision{};};
struct FtnControlIntent {FtnScanIntent text;bool password_subject{};};
struct FtnRescanItem {
    Identifier128 request;std::int64_t before{},arrival{};FtnScanIntent text;
    std::optional<FtnPackedMessage> original;FtnAddress original_origin;
};
// Host-only, secret-free admission observation. It is never decoded from owner
// IPC or BinkP and cannot confer ordinary File Manager mutation authority.
struct FtnFileRequest {
    Identifier128 operation,tic_receipt,payload_receipt;
    std::int64_t configuration_revision{},area_revision{};
    std::string network,link,tag,filename,replaces,origin,from;
    FileDescriptions descriptions;
    FilePayloadIdentity tic,payload;
    std::uint32_t crc{};
    bool password_verified{},authenticated_same_link{};
};
// Complete original forwarding intent, including an explicit empty target set.
// Password-free template plus protected-credential generation and exact output
// hashes permit byte-identical reconstruction without storing a clear secret.
struct FtnFileForwardTarget {
    std::string link,address;FtnDigest credential_generation{},peer_identity{};
    FtnOutboundJob payload,tic;
};
struct FtnFileForwardPlan {
    Identifier128 publication;std::int64_t created{};
    std::string network,local_address,metadata_template;
    FtnDigest source_peer_identity{};
    std::vector<FtnFileForwardTarget> targets;
};
std::vector<std::uint8_t> encode_ftn_file_forward_plan(const FtnFileForwardPlan&);
std::optional<FtnFileForwardPlan> decode_ftn_file_forward_plan(const std::vector<std::uint8_t>&);
std::string ftn_forward_peer_identity_material(const FtnLink&);
enum class FtnFilePhase : std::uint8_t { reserved,stage_bound,staged,recovery_bound,displaced,placed,completed };
struct FtnFilePublication {
    FtnFileRequest request;FileAreaConfiguration area;
    std::optional<ManagedFile> displaced;
    std::string stage_name,recovery_key,reason;
    FtnFilePhase phase{FtnFilePhase::reserved};
    std::optional<FileObjectIdentity> stage_object,recovery_object;
    std::optional<FilePayloadIdentity> staged,recovered,placed;
    FileId result;
    std::int64_t created{};
};
// A native rejection is separate from transport custody. The owner reviews an
// opaque exact-state digest; no native identity/proof/success assertion is
// accepted from the owner transport.
struct FtnTicRejection {
    Identifier128 ingress,attempt;std::int64_t configuration_revision{};
    FilePayloadIdentity native;std::string reason;
    std::string claimed_from,echo,payload;
};
struct FtnTicReview {
    FtnIngressReceipt receipt;FilePayloadIdentity native;
    std::int64_t configuration_revision{},rejection_sequence{},rejected_revision{};
    std::string reason;FtnDigest review{};
    std::string claimed_from,echo,payload;
    std::optional<FtnFilePublication> publication;
    std::optional<FtnDigest> forward_plan;
    std::optional<FtnTicRetryRecord> retry;
    std::vector<FtnSpoolObject> spool;
};
struct FtnFileAuthority {Identifier128 attempt;std::int64_t configuration_revision{},area_revision{},file_catalog_revision{};};
class IFtnMailStorage {
public:
    virtual StorageResult<std::vector<FtnRetainedOriginal>> ftn_retention_candidates(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageResult<FtnExpiryIntent> prepare_ftn_expiry(const MessageConfigurationActor&,const FtnExpiryIntent&)=0;
    virtual StorageResult<std::optional<FtnExpiryIntent>> pending_ftn_expiry(const MessageConfigurationActor&,const Identifier128& ingress)=0;
    virtual StorageStatus reconcile_ftn_expiry(const MessageConfigurationActor&,const FtnExpiryIntent&,FtnExpiryOutcome)=0;
    virtual StorageStatus execute_ftn_expiry(const MessageConfigurationActor&,std::shared_ptr<FtnRetentionRemoval>)=0;
    virtual StorageStatus reject_ftn_tic(const MessageConfigurationActor&,const FtnTicRejection&)=0;
    virtual StorageResult<std::optional<FtnTicReview>> review_ftn_tic(const MessageConfigurationActor&,const Identifier128& ingress)=0;
    virtual StorageResult<FtnTicRetryRecord> request_ftn_tic_retry(const MessageConfigurationActor&,const FtnTicRetryRequest&)=0;
    virtual StorageResult<std::vector<FtnTicRetryRecord>> pending_ftn_tic_retries(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageResult<FtnTicReview> check_ftn_tic_retry(const MessageConfigurationActor&,const Identifier128& operation)=0;
    virtual StorageStatus finish_ftn_tic_retry(const MessageConfigurationActor&,const Identifier128& operation,bool processed,std::string_view reason)=0;
    virtual StorageResult<FtnReprocessRecord> request_ftn_reprocess(const MessageConfigurationActor&,const FtnReprocessRequest&)=0;
    virtual StorageResult<std::optional<FtnReprocessRecord>> latest_ftn_reprocess(const MessageConfigurationActor&,const Identifier128& ingress)=0;
    virtual StorageResult<std::vector<FtnReprocessRecord>> pending_ftn_reprocess(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    // Host-only admission/completion for the existing sole native processor.
    virtual StorageResult<FtnIngressReceipt> check_ftn_reprocess(const MessageConfigurationActor&,const Identifier128& operation)=0;
    virtual StorageStatus finish_ftn_reprocess(const MessageConfigurationActor&,const Identifier128& operation,bool processed,std::string_view reason)=0;
    virtual StorageStatus record_ftn_held_message(const MessageConfigurationActor&,const FtnHeldMessage&)=0;
    virtual StorageResult<std::vector<FtnHeldMessage>> held_ftn_messages(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageResult<FtnRecipientResolution> resolve_ftn_recipient(const MessageConfigurationActor&,const FtnRecipientResolutionRequest&)=0;
    virtual StorageResult<FtnHeldReview> ftn_held_review(const MessageConfigurationActor&,const FtnHeldReviewQuery&)=0;
    virtual StorageResult<FtnHeldRecipients> ftn_held_recipients(const MessageConfigurationActor&,const FtnHeldRecipientQuery&)=0;
    virtual StorageResult<FtnHatchRecord> prepare_ftn_hatch(const MessageConfigurationActor&,const FtnHatchRequest&,const FilePayloadIdentity&,std::uint32_t crc)=0;
    virtual StorageResult<FtnHatchRecord> ftn_hatch(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageResult<FtnHatchRecord> bind_ftn_hatch_preparer(const MessageConfigurationActor&,const Identifier128&,const FileObjectIdentity&)=0;
    virtual StorageResult<std::vector<FtnHatchRecord>> pending_ftn_hatches(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageResult<std::vector<FtnHatchRecord>> ftn_hatches_for_file(const MessageConfigurationActor&,FileId,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageResult<FtnHatchRecord> commit_ftn_hatch(const MessageConfigurationActor&,const Identifier128&,const std::vector<FtnOutboundJob>&)=0;
    virtual StorageStatus hold_ftn_hatch(const MessageConfigurationActor&,const Identifier128&,std::string_view reason)=0;
    virtual ~IFtnMailStorage()=default;
    virtual StorageResult<FtnSpoolObject> prepare_ftn_spool(const MessageConfigurationActor&,std::int64_t revision,const FtnOutboundJob&,const Identifier128& attempt={})=0;
    virtual StorageResult<FtnSpoolObject> bind_ftn_spool(const MessageConfigurationActor&,const Identifier128&,const FileObjectIdentity&,const Identifier128& attempt={})=0;
    virtual StorageResult<FtnSpoolObject> verify_ftn_spool(const MessageConfigurationActor&,const Identifier128&,const FilePayloadIdentity&,const Identifier128& attempt={})=0;
    // Host records only a native, current-config TIC inspection. Empty payload
    // means a conclusive admission rejection, never a transient read failure.
    virtual StorageStatus index_ftn_tic(const MessageConfigurationActor&,const Identifier128&,std::int64_t revision,std::string_view payload,std::string_view reason)=0;
    // Durable receipt ownership plus current pending-TIC fence. False is a
    // retained ambiguity/uninspected TIC; it is never permission to toss.
    virtual StorageResult<bool> claim_ftn_message(const MessageConfigurationActor&,const Identifier128&,std::int64_t revision)=0;
    virtual StorageResult<FtnFilePublication> prepare_ftn_file(const MessageConfigurationActor&,const FtnFileRequest&,const std::optional<FtnFileForwardPlan>& plan={},const Identifier128& attempt={})=0;
    virtual StorageResult<FtnFileAuthority> ftn_file_authority(const MessageConfigurationActor&,const Identifier128& publication,const Identifier128& attempt={})=0;
    virtual StorageResult<FtnFileForwardPlan> ftn_file_forward_plan(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageResult<FtnFilePublication> ftn_file_publication(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageResult<std::vector<Identifier128>> pending_ftn_files(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageResult<FtnFilePublication> advance_ftn_file(const MessageConfigurationActor&,const Identifier128&,FtnFilePhase,const FilePayloadIdentity&,const Identifier128& attempt={})=0;
    virtual StorageResult<FtnFilePublication> commit_ftn_file(const MessageConfigurationActor&,const Identifier128&,const std::vector<FtnOutboundJob>&,const Identifier128& attempt={})=0;
    virtual StorageStatus hold_ftn_file(const MessageConfigurationActor&,const Identifier128&,std::string_view reason)=0;
    virtual StorageResult<FtnConfiguration> ftn_configuration(const MessageConfigurationActor&)=0;
    virtual StorageResult<FtnAreaFixResult> apply_ftn_areafix(const MessageConfigurationActor&,const FtnAreaFixRequest&)=0;
    virtual StorageResult<FtnImportResult> record_ftn_provider_response(const MessageConfigurationActor&,const FtnImportRequest&)=0;
    virtual StorageResult<std::vector<FtnControlIntent>> pending_ftn_control(const MessageConfigurationActor&,const Identifier128& after,std::size_t maximum)=0;
    virtual StorageStatus commit_ftn_control(const MessageConfigurationActor&,std::int64_t revision,const FtnControlIntent&,const FtnOutboundJob&)=0;
    virtual StorageResult<std::optional<FtnRescanItem>> next_ftn_rescan(const MessageConfigurationActor&,const Identifier128& after)=0;
    virtual StorageStatus commit_ftn_rescan(const MessageConfigurationActor&,std::int64_t revision,const FtnRescanItem&,const std::optional<FtnOutboundJob>&)=0;
    // Installation-owner authority is host-created, never decoded from BinkP.
    virtual StorageResult<FtnIngressReceipt> begin_ftn_ingress(const MessageConfigurationActor&,const FtnIngressReceipt&)=0;
    virtual StorageResult<FtnIngressReceipt> ftn_ingress_receipt(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageResult<std::optional<FilePayloadIdentity>> ftn_ingress_checkpoint(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageStatus checkpoint_ftn_ingress(const MessageConfigurationActor&,const Identifier128&,std::uint64_t expected,const FilePayloadIdentity&)=0;
    virtual StorageStatus advance_ftn_ingress(const MessageConfigurationActor&,const Identifier128&,
        std::uint64_t expected,std::uint64_t received,std::optional<FtnDigest>,FtnIngressState,std::string_view reason)=0;
    virtual StorageResult<std::vector<FtnIngressReceipt>> pending_ftn_ingress(const MessageConfigurationActor&,std::size_t maximum)=0;
    virtual StorageResult<std::vector<FtnIngressReceipt>> pending_ftn_ingress_after(const MessageConfigurationActor&,std::size_t maximum,const Identifier128& after)=0;
    // At most 17 rows: the native worker refuses a full page rather than hiding
    // additional candidates. Scope comes from the saved TIC custody receipt.
    virtual StorageResult<std::vector<FtnIngressReceipt>> ftn_payload_candidates(const MessageConfigurationActor&,const Identifier128& tic,std::string_view filename)=0;
    virtual StorageResult<FtnImportResult> import_ftn_message(const MessageConfigurationActor&,const FtnImportRequest&)=0;
    virtual StorageResult<std::vector<FtnTossProgress>> ftn_toss_progress(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageResult<bool> known_ftn_message(const MessageConfigurationActor&,std::string_view network,
        std::string_view tag,std::string_view origin,const FtnDigest& identity,const FtnDigest& content)=0;
    virtual StorageResult<std::vector<FtnOutboundJob>> ftn_jobs_for_operation(const MessageConfigurationActor&,const Identifier128&)=0;
    virtual StorageStatus queue_ftn_jobs(const MessageConfigurationActor&,std::int64_t expected_configuration_revision,const std::vector<FtnOutboundJob>&)=0;
    virtual StorageResult<std::vector<FtnOutboundJob>> claim_ftn_jobs(const MessageConfigurationActor&,std::string_view network,std::string_view link,
        const Identifier128& generation,const Identifier128& claim,std::size_t maximum,bool include_hold)=0;
    virtual StorageStatus finish_ftn_job(const MessageConfigurationActor&,const Identifier128& job,const Identifier128& claim,FtnOutboundState,std::string_view reason)=0;
    virtual StorageStatus recover_ftn_claims(const MessageConfigurationActor&,const Identifier128& generation)=0;
    virtual StorageResult<std::vector<FtnExternalPublication>> prepare_ftn_external_handoff(const MessageConfigurationActor&,std::int64_t,const Identifier128&) {return {{StorageErrorCode::invalid_argument,"external handoff unavailable"},{}};}
    virtual StorageResult<std::vector<FtnExternalPublication>> pending_ftn_external_handoff(const MessageConfigurationActor&,const Identifier128& = {}) {return {{StorageErrorCode::invalid_argument,"external handoff unavailable"},{}};}
    virtual StorageStatus finish_ftn_external_handoff(const MessageConfigurationActor&,const Identifier128&,bool) {return {StorageErrorCode::invalid_argument,"external handoff unavailable"};}
    virtual StorageStatus release_ftn_external_handoff(const MessageConfigurationActor&,const Identifier128&) {return {StorageErrorCode::invalid_argument,"external handoff unavailable"};}
    virtual StorageResult<std::vector<FtnScanIntent>> pending_ftn_scan(const MessageConfigurationActor&,std::string_view network,std::int64_t after,std::size_t maximum)=0;
    virtual StorageStatus commit_ftn_scan(const MessageConfigurationActor&,std::int64_t revision,const FtnScanIntent&,const std::vector<FtnOutboundJob>&)=0;
    virtual StorageResult<FtnSerialState> ftn_serial_state(const MessageConfigurationActor&)=0;
    virtual StorageResult<FtnQueuePage> ftn_queue_page(const MessageConfigurationActor&,std::uint32_t offset)=0;
    // Narrow authenticated-caller inspection. Existing local-owner commands
    // remain unchanged; neither method accepts a mutation or returns secrets.
    virtual StorageResult<FtnQueuePage> sysop_ftn_queues(const MessageActor&,std::string_view,std::uint32_t) {return {{StorageErrorCode::permission_denied,"Sysop FTN inspection unavailable"},{}};}
    virtual StorageResult<FtnHeldReview> sysop_ftn_held(const MessageActor&,std::string_view,Identifier128) {return {{StorageErrorCode::permission_denied,"Sysop held inspection unavailable"},{}};}
    virtual StorageResult<bool> ftn_outbound_ready(const MessageConfigurationActor&,std::string_view,std::string_view){return {{StorageErrorCode::invalid_argument,"outbound readiness unavailable"},{}};}
    // Host-only restore publication. Unknown prior history sets a durable hold;
    // this operation can raise a floor/hold, never clear an unresolved hold.
    virtual StorageStatus preserve_ftn_serial_history(const MessageConfigurationActor&,const std::optional<FtnSerialState>&,bool restored_predecessor=false)=0;
};
}
