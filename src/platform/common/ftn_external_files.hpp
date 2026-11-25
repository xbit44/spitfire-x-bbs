#pragma once
#include "sfx/application/ftn_external.hpp"
#include <filesystem>
#include <functional>

namespace sfx::platform::external_ftn {
// All paths are relative to an explicit board root. No database is opened by
// the recorder; its owned local metadata may be consumed when SFX starts.
void prepare_profile(const std::filesystem::path& root,std::string_view network);
struct RunProfile {std::string id,configuration_path;};
RunProfile prepare_run(const std::filesystem::path& root,std::string_view source_profile,std::string_view recorder);
application::FtnExternalReceiveRecord record_run_completed(const std::filesystem::path& root,std::string_view run,
    std::string relative_file,bool protected_session,const std::vector<application::FtnAddress>& remote,
    const std::function<void(std::string_view)>& boundary={});
// Includes consumed TIC records: a TIC can arrive before its payload. Every
// index entry is checked against the original record and frozen run manifest.
std::vector<application::FtnExternalReceiveRecord> tic_records_for_payload(const std::filesystem::path& root,std::string_view filename,
    const std::function<void(std::string_view,std::string_view)>& report={});
application::FtnExternalReceiveRecord record_completed(
    const std::filesystem::path& root,std::string_view network,std::string relative_file,
    bool protected_session,const std::vector<application::FtnAddress>& remote);
std::vector<application::FtnExternalReceiveRecord> pending_records(
    const std::filesystem::path& root,std::string_view network,std::size_t maximum=64,
    const std::function<void(std::string_view)>& report={},std::string_view after={});
void read_completed(const std::filesystem::path& root,const application::FtnExternalReceiveRecord&,
    const std::function<void(std::string_view)>& consume,const std::function<bool()>& running);
void mark_record_consumed(const std::filesystem::path& root,const application::FtnExternalReceiveRecord&);
// Read-only diagnostics. A missing/unusable hook record never creates custody.
// Completed inbox only; stock partial downloads and private logs are excluded.
void audit_inbox(const std::filesystem::path& root,
    const std::function<void(std::string_view,std::string_view)>& report);
std::string read_small(const std::filesystem::path& root,std::string_view relative,std::size_t maximum=8192);
// Exact replay is accepted; an existing different object is retained/refused.
void publish_once(const std::filesystem::path& root,std::string_view relative,std::string_view bytes,
    const std::function<void(std::string_view)>& boundary={});
struct PublicationResult {bool published{},busy{},uncertain{};};
using ReadOutbound=std::function<void(const application::FtnOutboundJob&,const std::function<void(std::string_view)>&)>;
// Caller supplies an exact stored batch and verified canonical reads. Busy lock
// remains across all copies/FLOs and the database handoff commit. Fault seam is
// host-only, never IPC. No destructor deletes an interrupted publication lock.
PublicationResult publish_handoff(const std::filesystem::path& root,application::IFtnMailStorage&,
    const std::vector<application::FtnExternalPublication>&,const ReadOutbound&,
    const std::function<bool()>& running,const std::function<void(std::string_view)>& boundary={});
}
