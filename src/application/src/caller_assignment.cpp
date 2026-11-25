#include "sfx/application/caller_assignment.hpp"
#include "sfx/application/hostname_lookup.hpp"

#include <utility>
#include <limits>

namespace sfx::application {
namespace {
const char* label(CallerStage value) noexcept {
    switch (value) {
        case CallerStage::transport: return "Transport setup";
        case CallerStage::graphics: return "ANSI selection";
        case CallerStage::identity: return "Caller login";
        case CallerStage::authentication: return "Authentication";
        case CallerStage::registration: return "Registration";
        case CallerStage::system_password: return "System Password";
        case CallerStage::caller_information: return "Caller information";
        case CallerStage::main_menu: return "Main Menu";
        case CallerStage::cleanup: return "Disconnecting";
    }
    return "Caller login";
}
const char* detail(CallerDiagnostic value) noexcept {
    switch (value) {
        case CallerDiagnostic::none: return "Connection assigned";
        case CallerDiagnostic::transport_failure: return "Transport ended";
        case CallerDiagnostic::authentication_ended: return "Authentication ended";
        case CallerDiagnostic::workflow_failure: return "Caller workflow ended";
        case CallerDiagnostic::session_failure: return "Caller session failed";
        case CallerDiagnostic::cleanup_failure: return "Cleanup failed; node unavailable";
    }
    return "Caller session failed";
}
}
CallerAssignment::CallerAssignment(NodeLease lease, FourNodePool& pool, IClock& clock,
    bool ssh, std::string source)
    : cancellation(*lease.cancellation()), cleanup_health(std::make_shared<CallerCleanupHealth>()),
      lease_(std::move(lease)), identity_(lease_.identity()), callbacks_(lease_.callbacks()), pool_(pool), clock_(clock),
      started_(clock.monotonic_millis()) {
    node_.number = lease_.identity().node;
    node_.assignment_sequence = lease_.identity().generation;
    node_.screen_owner = identity_;
    node_.available = node_.enabled = node_.screen_active = true;
    node_.caller_connected = true;
    node_.state = core::NodeState::connecting;
    node_.connection = ssh ? "SSH" : "Telnet";
    node_.source_ip = std::move(source);
    node_.activity = label(CallerStage::transport);
    node_.details = detail(CallerDiagnostic::none);
    node_.screen = mirror_.snapshot();
}
void CallerAssignment::stage(CallerStage value) noexcept {
    try { (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return;
        node_.activity = label(value);
    }); } catch (...) { cleanup_health->failed.store(true); }
}
void CallerAssignment::menu_entered(MenuId menu) noexcept {
    // Fixed trusted names only. Never infer location from caller artwork or
    // retain a callback that can publish into the next occupant of this node.
    static constexpr const char* names[]={"Main Menu","Message Menu","File Menu",
        nullptr,nullptr,"Door Menu","Bulletin Menu",
        "Door Bulletin Menu","Chat Menu","Your Info / Settings","File Groups",
        "File Areas","Batch Transfer Menu","QWK / Offline Mail"};
    const auto index=static_cast<std::size_t>(menu);
    if(index>=std::size(names)||!names[index])return;
    try{(void)callbacks_.invoke([&]{
        std::lock_guard<std::mutex> lock(mutex_);
        if(!retired_&&node_.caller_connected&&node_.account_id>0&&node_.state==core::NodeState::online)
            node_.activity=names[index];
    });}catch(...){/* Optional presentation never ends a caller session. */}
}
std::uint64_t CallerAssignment::begin_door(DoorId id,std::string_view name,DoorExecutionKind kind) noexcept {
    if(id<1||id>999||!valid_door_text(name,64))return 0;
    std::uint64_t result{};
    try{std::string activity(name),details=(kind==DoorExecutionKind::rlogin?"Native outbound RLogin; Door ":kind==DoorExecutionKind::telnet?"Native outbound Telnet; Door ":"Local program; Door ")+std::to_string(id);
        (void)callbacks_.invoke([&]{std::lock_guard<std::mutex> lock(mutex_);
        if(retired_||!node_.caller_connected||!node_.account_id||node_.door_id||door_sequence_==UINT64_MAX)return;
        node_.activity=std::move(activity);node_.details=std::move(details);
        node_.door_id=id;node_.door_launch_sequence=++door_sequence_;terminated_door_=0;node_.state=core::NodeState::door;result=node_.door_launch_sequence;
    });}catch(...){return 0;}return result;
}
void CallerAssignment::end_door(std::uint64_t launch) noexcept {
    try{std::lock_guard<std::mutex> lock(mutex_);if(retired_||!launch||node_.door_launch_sequence!=launch)return;
        node_.door_id=0;node_.door_launch_sequence=0;terminated_door_=0;
        if(node_.caller_connected){node_.state=core::NodeState::online;node_.activity="Door Menu";node_.details="Authenticated caller";}
    }catch(...){cleanup_health->failed=true;}
}
bool CallerAssignment::request_door_termination(std::uint64_t launch) noexcept {
    bool accepted{};try{(void)callbacks_.invoke([&]{std::lock_guard<std::mutex> lock(mutex_);if(!retired_&&launch&&node_.door_id&&node_.door_launch_sequence==launch){terminated_door_=launch;accepted=true;}});}catch(...){}return accepted;
}
bool CallerAssignment::door_termination_requested(std::uint64_t launch) const noexcept {
    try{std::lock_guard<std::mutex> lock(mutex_);return launch&&terminated_door_==launch;}catch(...){return true;}
}
void CallerAssignment::output(std::string_view bytes) {
    (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return;
        mirror_.consume(bytes);
        node_.screen = mirror_.snapshot();
    });
}
void CallerAssignment::resize(std::uint16_t columns, std::uint16_t rows) {
    if (!columns || !rows || columns > 4096 || rows > 4096) return;
    (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return;
        node_.terminal_columns = columns;
        node_.terminal_rows = rows;
    });
}
void CallerAssignment::payload_activity(bool received) noexcept {
    try { (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_ || !node_.caller_connected) return;
        auto& value = received ? node_.receive_activity : node_.send_activity;
        if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
    }); } catch (...) { /* Optional presentation must not alter caller success. */ }
}
void CallerAssignment::encrypted_transport_established() noexcept {
    try { (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!retired_ && node_.caller_connected && node_.connection == "SSH")
            node_.ssh_encrypted = true;
    }); } catch (...) { }
}
void CallerAssignment::transport_closed() noexcept {
    // Owner cleanup remains valid after cancellation has sealed callbacks.
    std::lock_guard<std::mutex> lock(mutex_);
    node_.caller_connected = node_.ssh_encrypted = false;
    node_.account_id = 0;
    node_.door_id=0;node_.door_launch_sequence=0;terminated_door_=0;
}
void CallerAssignment::authenticated(std::string name, AccountId account_id) {
    const auto normalized = normalize_account_name(name);
    if (!normalized || *normalized != name || account_id<=0) return;
    (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return;
        node_.caller_handle = std::move(name);
        node_.account_id = account_id;
        node_.state = core::NodeState::online;
        node_.activity = label(CallerStage::main_menu);
        node_.details = "Authenticated caller";
    });
}
void CallerAssignment::hostname(const NodeAssignmentIdentity& identity, std::string_view source,
    std::string_view hint) noexcept {
    if (!(identity == identity_)) return;
    const auto name = normalize_hostname_hint(hint);
    if (!name || name->view() != hint) return;
    try { (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!retired_ && node_.screen_active && node_.source_ip == source)
            node_.hostname = hint; // metadata only; no mirror/activity change
    }); } catch (...) { /* optional display failure never poisons caller cleanup */ }
}
void CallerAssignment::diagnose(CallerDiagnostic value) noexcept {
    try { (void)callbacks_.invoke([&] {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return;
        diagnostic_ = value;
        node_.details = detail(value);
    }); } catch (...) { cleanup_health->failed.store(true); }
}
void CallerAssignment::cancel() {
    stopping.store(true);
    (void)pool_.cancel(identity_);
}
bool CallerAssignment::retire(bool clean) {
    clean = clean && !cleanup_health->failed.load();
    // Owner cleanup is NOT a callback: cancellation deliberately closes the
    // callback gate, but must not prevent clearing this still-owned assignment.
    // Seal under the snapshot lock, drop it, then release the lease. Never take
    // the pool lock while holding this lock (callbacks use the reverse order).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (retired_) return false;
        retired_ = true;
        AlphaNodeStatus reset;
        reset.number = node_.number;
        reset.assignment_sequence = node_.assignment_sequence;
        reset.enabled = true;
        reset.available = clean;
        reset.state = clean ? core::NodeState::ready : core::NodeState::offline;
        reset.activity = clean ? "Idle" : "Unavailable";
        reset.details = clean ? "Ready for connection" : detail(CallerDiagnostic::cleanup_failure);
        mirror_.clear();
        reset.screen = mirror_.snapshot();
        node_ = std::move(reset);
        diagnostic_ = clean ? CallerDiagnostic::none : CallerDiagnostic::cleanup_failure;
    }
    // Failed cleanup leaves the lease occupied/quarantined, never falsely free.
    if (!clean) { cancel(); return false; }
    return lease_.release_after_cleanup();
}
AlphaNodeStatus CallerAssignment::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = node_;
    if (result.screen_active) result.connected_seconds = (clock_.monotonic_millis() - started_) / 1000;
    return result;
}
CallerDiagnostic CallerAssignment::diagnostic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnostic_;
}
} // namespace sfx::application
