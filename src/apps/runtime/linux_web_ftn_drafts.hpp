#pragma once
#include "linux_web_ftn_records.hpp"
#include "linux_web_ftn_transfer.hpp"
#include "linux_web_ftn_import.hpp"
#include <map>
#include <mutex>

namespace sfx::apps::runtime::web {
enum class WebFtnDraftState { editing, review, submitted };
struct WebFtnDraft {
    application::FtnTransferToken handle{},operation{};
    std::uint64_t edit_revision{1};
    WebFtnDraftState state{WebFtnDraftState::editing};
    application::FtnConfiguration configuration;
    std::optional<application::FtnRetentionPreview> review;
    std::optional<application::FtnConfigurationResponse> result;
};
// Bounded, session-owned in-memory drafts. Only protected credential envelopes
// are retained. Each reviewed operation can issue at most one native commit.
class WebFtnDrafts final {
public:
    using TokenFactory=std::function<application::FtnTransferToken()>;
    using Now=std::function<std::uint64_t()>;
    WebFtnDrafts(FtnExchange,FtnSeal,TokenFactory,Now,FtnListReader=read_ftn_echo_source);
    ~WebFtnDrafts();
    WebFtnDraft open(const std::string& owner);
    WebFtnDraft read(const std::string& owner,application::FtnTransferToken);
    WebFtnDraft edit(const std::string& owner,application::FtnTransferToken,std::uint64_t,const nlohmann::json&);
    WebFtnDraft edit_batch(const std::string& owner,application::FtnTransferToken,std::uint64_t,const nlohmann::json&);
    WebFtnDraft prepare(const std::string& owner,application::FtnTransferToken,std::uint64_t);
    WebFtnDraft commit(const std::string& owner,application::FtnTransferToken,application::FtnTransferToken operation);
    WebFtnDraft check_result(const std::string& owner,application::FtnTransferToken);
    WebFtnDraft cancel_review(const std::string& owner,application::FtnTransferToken);
    WebFtnListSnapshot start_list(const std::string& owner,application::FtnTransferToken,std::uint64_t,std::string network,std::string source,bool url,bool cp437,std::string selected_source={});
    WebFtnListSnapshot read_list(const std::string& owner,application::FtnTransferToken,application::FtnTransferToken list);
    void cancel_list(const std::string& owner,application::FtnTransferToken,application::FtnTransferToken list);
    application::FtnEchoImportPlan import_preview(const std::string& owner,application::FtnTransferToken,std::uint64_t,application::FtnTransferToken list,const WebFtnImportCatalog&,const std::vector<std::string>&,application::FtnEchoImportRequirements,bool update);
    WebFtnDraft import_apply(const std::string& owner,application::FtnTransferToken,std::uint64_t,application::FtnTransferToken list,const WebFtnImportCatalog&,const std::vector<std::string>&,application::FtnEchoImportRequirements,bool update);
    void discard(const std::string& owner,application::FtnTransferToken);
    void clear();
private:
    struct Entry {std::string owner;std::uint64_t last{};WebFtnDraft value;WebFtnPrepared prepared;std::shared_ptr<WebFtnListJob> list,pending_list;};
    FtnExchange exchange_;FtnSeal seal_;TokenFactory token_;Now now_;
    FtnListReader list_reader_;
    std::mutex mutex_;std::map<application::FtnTransferToken,Entry> entries_;
    application::FtnTransferToken new_token();
    Entry& find(const std::string&,application::FtnTransferToken);
    WebFtnListSnapshot list_for(Entry&,application::FtnTransferToken);
    void expire();
};
} // namespace sfx::apps::runtime::web
