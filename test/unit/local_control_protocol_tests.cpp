#include "sfx/application/local_control_protocol.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace app = sfx::application;
unsigned assertions = 0;
app::AlphaRuntimeStatus four_callers();

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++assertions;
}

app::OperationalConfiguration configuration() {
    app::OperationalConfiguration value;
    value.bbs_time_zone = "Pacific Standard Time";
    value.security_policy_revision = 1;
    value.security_policy_hash =
        "71cb5a767e365786bbedaa779e4dc743473fb70c2b9dd3a99e4f5ce154062a95";
    return value;
}

void frame_tests() {
    app::LocalAccountResult protected_sysop{app::LocalAccountResultCode::last_sysop_protected};
    const auto protected_result=app::decode_account_result(app::encode_account_result(protected_sysop));
    require(protected_result && protected_result->code==protected_sysop.code && !protected_result->committed,
        "last-Sysop reason crosses protected IPC without becoming a generic conflict");
    protected_sysop.committed=true;
    require(app::encode_account_result(protected_sysop).empty(),"last-Sysop refusal cannot claim a commit");
    protected_sysop={app::LocalAccountResultCode::system_sysop_protected};
    const auto designated=app::decode_account_result(app::encode_account_result(protected_sysop));
    require(designated && designated->code==protected_sysop.code,"System Sysop assignment refusal has a distinct typed IPC reason");
    protected_sysop.committed=true;
    require(app::encode_account_result(protected_sysop).empty(),"System Sysop assignment refusal cannot claim a commit");
    app::BotGateFeedStatus feed;
    auto encoded_feed=app::encode_botgate_feed_status(feed);
    require(!encoded_feed.empty() && app::decode_botgate_feed_status(encoded_feed).has_value(),"degraded feed status roundtrip");
    feed.available=true; feed.last_success=1700000000; feed.last_attempt=1700000001;
    feed.entries=150000; feed.bytes_received=8000000; feed.updating=true; feed.result="feed_downloading";
    feed.enabled=true;feed.telnet=true;feed.ssh=true;feed.rules_ready=true;feed.protection_counts[4]=UINT64_MAX;
    feed.geo_summary="brazil.txt, china.txt";feed.rule_counts[0]=45;
    encoded_feed=app::encode_botgate_feed_status(feed);
    const auto decoded_feed=app::decode_botgate_feed_status(encoded_feed);
    require(decoded_feed && decoded_feed->entries==feed.entries && decoded_feed->updating &&
        decoded_feed->last_success==feed.last_success,"active feed progress roundtrip");
    require(decoded_feed && decoded_feed->protection_counts[4]==UINT64_MAX && decoded_feed->geo_summary==feed.geo_summary &&
        decoded_feed->rule_counts[0]==45 && decoded_feed->rules_ready,"Runtime protection counters and rule health roundtrip");
    encoded_feed.push_back(0); require(!app::decode_botgate_feed_status(encoded_feed),"feed status rejects trailing bytes");
    feed.result="raw\ntext"; require(app::encode_botgate_feed_status(feed).empty(),"feed diagnostics are closed printable identifiers");
    app::LocalControlFrame frame;
    frame.type = app::LocalControlMessageType::status_request;
    frame.request_id = 7;
    frame.payload = {1, 2, 3};
    const auto bytes = app::encode_local_control_frame(frame);
    const auto decoded = app::decode_local_control_frame(bytes);
    require(decoded.code == app::LocalControlFrameDecodeCode::complete && decoded.frame,
        "valid frame decodes");
    require(decoded.frame->request_id == 7 && decoded.frame->payload == frame.payload,
        "frame identity and payload round trip");

    auto incompatible = bytes;
    incompatible[4] = 1;
    require(app::decode_local_control_frame(incompatible).code ==
        app::LocalControlFrameDecodeCode::incompatible_version, "major mismatch rejects");
    incompatible = bytes;
    incompatible[6] = 1;
    require(app::decode_local_control_frame(incompatible).code ==
        app::LocalControlFrameDecodeCode::incompatible_version,
        "unsupported minor version rejects before payload handling");
    incompatible = bytes;
    incompatible[4] = 3;
    require(app::decode_local_control_frame(incompatible).code ==
        app::LocalControlFrameDecodeCode::incompatible_version,
        "frozen version 3 peer rejects before version 4 settings/status payload handling");
    for (unsigned version = 4; version <= app::kAlphaLocalControlVersion.major + 1U; ++version) {
        if (version == app::kAlphaLocalControlVersion.major) continue;
        incompatible = bytes; incompatible[4] = static_cast<std::uint8_t>(version);
        require(app::decode_local_control_frame(incompatible).code ==
            app::LocalControlFrameDecodeCode::incompatible_version,
            "matched administration protocol rejects older and future pairs before payload parsing");
    }
    auto unknown = bytes;
    unknown[8] = 103;
    require(app::decode_local_control_frame(unknown).code ==
        app::LocalControlFrameDecodeCode::unknown_message, "unknown message rejects");
    for(const auto type:{app::LocalControlMessageType::door_bulletin_query_request,app::LocalControlMessageType::door_bulletin_query_response,app::LocalControlMessageType::door_bulletin_change_request,app::LocalControlMessageType::door_bulletin_change_response}){auto bulletin=frame;bulletin.type=type;const auto back=app::decode_local_control_frame(app::encode_local_control_frame(bulletin));require(back.frame&&back.frame->type==type,"appended Door Bulletin frame types round trip without changing existing numbers");}
    app::LocalDoorBulletinQuery query;query.kind=app::LocalDoorBulletinQueryKind::preview;query.id={999};query.revision=42;query.ansi=false;const auto query_bytes=app::encode_door_bulletin_query(query);const auto restored_query=app::decode_door_bulletin_query(query_bytes);require(restored_query&&restored_query->id.value==999&&restored_query->revision==42&&!restored_query->ansi,"preview request binds full-width ID and expected catalog revision");
    for (std::size_t n = 0; n < query_bytes.size(); ++n) {
        require(!app::decode_door_bulletin_query({query_bytes.begin(), query_bytes.begin() + n}),
            "truncated bulletin query rejects");
    }
    auto extra_query = query_bytes;
    extra_query.push_back(0);
    require(!app::decode_door_bulletin_query(extra_query), "bulletin query rejects trailing data");
    query.id = {1000};
    require(app::encode_door_bulletin_query(query).empty(), "bulletin query cannot truncate IDs above999");
    auto truncated = bytes;
    truncated.pop_back();
    require(app::decode_local_control_frame(truncated).code ==
        app::LocalControlFrameDecodeCode::length_mismatch, "truncated payload rejects");
    frame.payload.assign(app::kLocalControlMaximumPayloadBytes + 1U, 0);
    require(app::encode_local_control_frame(frame).empty(), "oversized payload does not encode");
}

template<class Decoder>
void require_strict_message(const std::vector<std::uint8_t>& bytes, Decoder decoder) {
    require(!bytes.empty() && static_cast<bool>(decoder(bytes)), "canonical administration message decodes");
    for (std::size_t length = 0; length < bytes.size(); ++length)
        require(!decoder(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + length)),
            "every administration truncation rejects");
    auto extra = bytes; extra.push_back(0);
    require(!decoder(extra), "administration trailing bytes reject");
}

void account_administration_protocol_tests() {
    for (const auto& request : {app::LocalThrottleClearRequest{app::RateLimitScope::source,"192.0.2.1"},
            {app::RateLimitScope::source,"2001:db8::1"},{app::RateLimitScope::account,"9223372036854775807"},
            {app::RateLimitScope::system_password,{}},{app::RateLimitScope::global,{}}}) {
        require_strict_message(app::encode_throttle_clear_request(request),app::decode_throttle_clear_request);
    }
    for (const auto& invalid : {app::LocalThrottleClearRequest{app::RateLimitScope::source,"example.test"},
            {app::RateLimitScope::source,"192.0.2.0/24"},{app::RateLimitScope::source,"192.000.2.1"},
            {app::RateLimitScope::account,"0"},{app::RateLimitScope::account,"01"},{app::RateLimitScope::account,"-1"},
            {app::RateLimitScope::account,"9223372036854775808"},{app::RateLimitScope::global,"192.0.2.1"},
            {static_cast<app::RateLimitScope>(4),{}}})
        require(app::encode_throttle_clear_request(invalid).empty(),"local throttle target must identify exactly one canonical scope");
    auto malformed_scope = app::encode_throttle_clear_request({app::RateLimitScope::global,{}});
    malformed_scope[0] = 255;
    require(!app::decode_throttle_clear_request(malformed_scope),"unknown throttle scope fails before dispatch");
    app::LocalAccountQuery query{0,"Caller",32};
    require_strict_message(app::encode_account_query(query), app::decode_account_query);
    query.account_id = 1;
    require(app::encode_account_query(query).empty(), "selected detail cannot carry list offset/filter");
    query = {1,"",0};
    auto query_bytes = app::encode_account_query(query); query_bytes[7] = 128;
    require(!app::decode_account_query(query_bytes), "unsigned account overflow rejects");
    query = {0,"bad\n",0};
    require(app::encode_account_query(query).empty(), "control text cannot enter account filter");

    app::LocalAccountRequest request;
    request.account_id = 1; request.expected_revision = 2;
    request.expected_policy_revision = 1; request.expected_reset_revision = 1;
    request.security_level = 10; request.name = "Caller One";
    request.profile.real_name = "Example Operator";
    for (unsigned action = 0; action <= static_cast<unsigned>(app::LocalAccountAction::save_user); ++action) {
        request.action = static_cast<app::LocalAccountAction>(action);
        request.password = "fixture-temporary-password"; request.confirmation = request.password;
        const auto bytes = app::encode_account_request(request);
        require_strict_message(bytes, app::decode_account_request);
        auto decoded = app::decode_account_request(bytes);
        require(decoded && app::encode_account_request(*decoded) == bytes &&
            decoded->password == request.password && decoded->confirmation == request.confirmation,
            "all actions round trip exactly and successful decoder erases only transient copies");
        app::erase_local_account_secrets(*decoded);
        require(decoded->password.empty() && decoded->confirmation.empty(), "owned decoded secrets clear");
    }
    request.password.assign(128, 'x'); request.confirmation = "mismatch";
    require_strict_message(app::encode_account_request(request), app::decode_account_request);
    request.password.push_back('x');
    require(app::encode_account_request(request).empty(), "overlong password refuses before framing");
    app::erase_local_account_secrets(request);
    auto bad_request = app::encode_account_request(request); bad_request[0] = 255;
    require(!app::decode_account_request(bad_request), "unknown administration action refuses");

    app::LocalAccountResult outcome{app::LocalAccountResultCode::revocation_failed,true,false,7};
    require_strict_message(app::encode_account_result(outcome), app::decode_account_result);
    outcome.sessions_revoked = true; outcome.committed = false;
    require(app::encode_account_result(outcome).empty(), "impossible credential result refuses");

    app::LocalAccountSnapshot snapshot;
    snapshot.code = app::LocalAccountResultCode::ok; snapshot.system_revision = 1;
    snapshot.policy.revision = 1; snapshot.policy.new_user_level = 10; snapshot.policy.sysop_threshold = 999;
    snapshot.policy.policy_hash = configuration().security_policy_hash;
    for (std::size_t i=0;i<app::kLocalAccountPageSize;++i) {
        app::AccountRecord account; account.account_id = static_cast<app::AccountId>(i+1);
        account.revision = 3; account.display_name = "Caller " + std::to_string(i+1);
        account.security_level = static_cast<std::uint16_t>(999); account.purge_allowed = true;
        account.profile.real_name = std::string(app::kRealNameMaximum,'x');
        account.profile.location = std::string(app::kLocationMaximum,'x');
        snapshot.accounts.push_back(account);
    }
    snapshot.has_more = true;
    snapshot.online=true;
    const auto page = app::encode_account_snapshot(snapshot);
    require_strict_message(page, app::decode_account_snapshot);
    require(page.size() < app::kLocalControlMaximumPayloadBytes &&
        app::encode_account_snapshot(*app::decode_account_snapshot(page)) == page,
        "full bounded account page round trips below frame ceiling");
    snapshot.accounts.push_back(snapshot.accounts.back());
    require(app::encode_account_snapshot(snapshot).empty(), "oversized account page refuses");
    snapshot.accounts.resize(1); snapshot.has_more = false;
    snapshot.temporary = app::TemporaryPasswordRecord{1,2,app::TemporaryPasswordState::active,100,259300,{}};
    require_strict_message(app::encode_account_snapshot(snapshot), app::decode_account_snapshot);
    snapshot.temporary->account_id = 2;
    require(app::encode_account_snapshot(snapshot).empty(), "temporary metadata must match selected account");
    snapshot.code = app::LocalAccountResultCode::unauthorized;
    require(app::encode_account_snapshot(snapshot).size() == 1,
        "refusal never discloses leftover account or credential metadata");
    require_strict_message(app::encode_account_snapshot(snapshot), app::decode_account_snapshot);

    app::LocalSecuritySnapshot catalog{app::LocalAccountResultCode::ok,{1,10,999,std::string(64,'a'),1,std::string(64,'a')},
        {{10,"New User",{},app::SecurityLevelState::active},{999,"Sysop",{},app::SecurityLevelState::active}},false};
    require_strict_message(app::encode_security_snapshot(catalog),app::decode_security_snapshot);
    require(app::decode_security_snapshot(app::encode_security_snapshot(catalog))->levels.size()==2,"live catalog carries existing definitions and defaults");
    catalog.levels.front().minutes_per_call=60;catalog.levels.front().minutes_per_day=180;
    const auto timed=app::decode_security_snapshot(app::encode_security_snapshot(catalog));
    require(timed && timed->levels.front().minutes_per_call==60U && timed->levels.front().minutes_per_day==180U &&
        !timed->levels.back().minutes_per_day,"matched private IPC preserves independent calling limits and 999 unlimited");
    catalog.levels.back().minutes_per_day=1;
    require(app::encode_security_snapshot(catalog).empty(),"999 finite limit rejected at private IPC boundary");
    app::LocalSecurityChange change{1,app::LocalSecurityAction::retire_level,10,999,999,999,{},{},std::string(64,'b')};
    require_strict_message(app::encode_security_change(change),app::decode_security_change);
    require(app::decode_security_change(app::encode_security_change(change))->affected_accounts_hash==change.affected_accounts_hash,"removal review fingerprint round trips");
    change.affected_accounts_hash.push_back('b');require(app::encode_security_change(change).empty(),"oversized fingerprint refused");
}

void payload_tests() {
    app::AlphaBootstrapRequest request;
    request.configuration = configuration();
    request.sysop_name = "Initial Sysop";
    request.sysop_password = "fixture-account-password";
    request.system_password = "fixture-system-password";
    const auto encoded = app::encode_bootstrap_request(request);
    auto decoded = app::decode_bootstrap_request(encoded);
    require(decoded && decoded->sysop_name == request.sysop_name &&
        decoded->sysop_password == request.sysop_password &&
        decoded->system_password == request.system_password,
        "bounded bootstrap payload round trips");
    app::erase_bootstrap_secrets(*decoded);
    require(decoded->sysop_password.empty() && decoded->system_password.empty(),
        "bootstrap secret erasure clears logical values");

    app::AlphaRuntimeStatus status;
    status.runtime_epoch.bytes[0] = 1;
    status.status_sequence = 1;
    status.state = app::AlphaRuntimePresentationState::setup_required;
    status.lifecycle.state = app::RuntimeState::online;
    status.lifecycle.owner_mode = app::RuntimeOwnershipMode::desktop;
    status.lifecycle.configured_nodes = 4;
    status.bbs_name = "Spitfire-X BBS";
    status.nodes.resize(4);
    for (std::size_t index = 0; index < status.nodes.size(); ++index) {
        status.nodes[index].number = static_cast<std::uint16_t>(index + 1U);
        status.nodes[index].state = sfx::core::NodeState::offline;
    }
    status.nodes[0].available = true;
    status.nodes[0].enabled = true;
    status.nodes[0].caller_handle = "Test Caller";
    status.nodes[0].connection = "Telnet";
    status.nodes[0].activity = "Main Menu";
    status.nodes[0].details = "Authenticated caller";
    status.nodes[0].source_ip = "127.0.0.1";
    status.nodes[0].assignment_sequence = 7;
    status.nodes[0].screen_active = true;
    status.nodes[0].state = sfx::core::NodeState::online;
    status.lifecycle.active_work.caller_sessions = 1;
    status.nodes[0].screen_owner = {status.runtime_epoch, 1, 7};
    status.nodes[0].screen.cells[0] = {'S', 0x0b};
    status.storage.state = app::StorageRuntimeState::ready;
    status.storage.schema_version = 1;
    status.storage.engine_version = "3.53.4";
    status.telnet_configured = true;
    status.telnet_bind = "0.0.0.0";
    status.telnet_port = 2323;
    status.ssh_configured = true;
    status.ssh_active = true;
    status.ssh_bind = "127.0.0.1";
    status.ssh_port = 2222;
    status.nodes[0].terminal_columns = 132;
    status.nodes[0].terminal_rows = 43;
    status.logs.push_back({1, 1, "INFO", "listener", "Telnet listener ready"});
    status.statistics.successful_logons = {true, 1, 9};
    status.diagnostic_code = "setup_required";
    const auto status_decoded = app::decode_runtime_status(app::encode_runtime_status(status));
    require(status_decoded && status_decoded->nodes.size() == 4 &&
        status_decoded->storage.schema_version == 1 && !status_decoded->telnet_active &&
        status_decoded->ssh_active && status_decoded->ssh_configured &&
        status_decoded->ssh_bind == "127.0.0.1" && status_decoded->ssh_port == 2222 &&
        status_decoded->nodes[0].terminal_columns == 132 && status_decoded->nodes[0].terminal_rows == 43 &&
        status_decoded->nodes[0].screen.cells[0].character == 'S' &&
        status_decoded->logs.size() == 1 &&
        status_decoded->statistics.successful_logons.total == 9,
        "safe runtime status round trips");

    app::AlphaRuntimeMetricSnapshot metrics;
    metrics.sqlite = {true, true, 14, 2, true, 9876};
    const auto metrics_bytes = app::encode_runtime_metrics(metrics);
    const auto metrics_decoded = app::decode_runtime_metrics(metrics_bytes);
    require(metrics_decoded && metrics_decoded->sqlite.available &&
        metrics_decoded->sqlite.page_reads_per_second == 14 &&
        metrics_decoded->sqlite.page_writes_per_second == 2 &&
        metrics_decoded->sqlite.cache_hit_basis_points == 9876,
        "typed SQLite pager metrics round trip without private database content");
    require(metrics_bytes.size() == 30,
        "metrics payload exposes only fixed-size availability and numeric fields");
    auto invalid_metrics = metrics_bytes;
    invalid_metrics[0] = 0;
    require(!app::decode_runtime_metrics(invalid_metrics),
        "inconsistent metrics availability fails closed");

    auto maximum_status = status;
    maximum_status.lifecycle.active_work.caller_sessions = 4;
    for (std::size_t index = 1; index < maximum_status.nodes.size(); ++index) {
        maximum_status.nodes[index] = maximum_status.nodes[0];
        maximum_status.nodes[index].number = static_cast<std::uint16_t>(index + 1U);
        maximum_status.nodes[index].screen_owner.node = maximum_status.nodes[index].number;
    }
    maximum_status.logs.clear();
    for (std::size_t index = 0; index < app::kControlPanelMaximumLogEntries; ++index) {
        maximum_status.logs.push_back({index + 1U, static_cast<std::int64_t>(index + 1U),
            "WARNING", "maximum-bounded-log-component", std::string(160, 'x')});
    }
    const auto maximum_encoded = app::encode_runtime_status(maximum_status);
    const auto maximum_decoded = app::decode_runtime_status(maximum_encoded);
    require(!maximum_encoded.empty() &&
            maximum_encoded.size() <= app::kLocalControlMaximumPayloadBytes &&
            maximum_decoded && maximum_decoded->nodes.size() == 4 &&
            maximum_decoded->logs.size() == app::kControlPanelMaximumLogEntries,
        "maximum configured logical-node shape and bounded log history fit one bounded status frame");

    const auto node_command = app::decode_node_command_request(
        app::encode_node_command_request({1, app::AlphaNodeCommandKind::disable}));
    require(node_command && node_command->node_number == 1 &&
        node_command->command == app::AlphaNodeCommandKind::disable,
        "bounded node command round trips");

    app::NodeAssignmentIdentity local_identity;
    local_identity.runtime_epoch.bytes[0] = 7;
    local_identity.node = 3;
    local_identity.generation = 29;
    const auto disconnect_bytes = app::encode_node_command_request({3,
        app::AlphaNodeCommandKind::disconnect, local_identity.runtime_epoch, 29});
    const auto disconnect = app::decode_node_command_request(disconnect_bytes);
    require(disconnect && disconnect->expected_generation == 29 &&
        disconnect->expected_epoch == local_identity.runtime_epoch,
        "disconnect preserves exact observed assignment identity");
    require(app::encode_node_command_request({3, app::AlphaNodeCommandKind::disconnect}).empty() &&
        app::encode_node_command_request({3, app::AlphaNodeCommandKind::enable,
            local_identity.runtime_epoch, 29}).empty(), "node commands reject missing or irrelevant identity");
    const auto guarded_idle = app::decode_node_command_request(app::encode_node_command_request({3,
        app::AlphaNodeCommandKind::disable_observed, local_identity.runtime_epoch, 0}));
    const auto guarded_caller = app::decode_node_command_request(app::encode_node_command_request({3,
        app::AlphaNodeCommandKind::disable_observed, local_identity.runtime_epoch, 29}));
    require(guarded_idle && guarded_caller && guarded_idle->expected_generation == 0 &&
        guarded_caller->expected_generation == 29 && guarded_caller->expected_epoch == local_identity.runtime_epoch &&
        guarded_caller->command == app::AlphaNodeCommandKind::disable_observed,
        "bulk disable round trips an observed idle node or a specific connected assignment");
    require(app::encode_node_command_request({3, app::AlphaNodeCommandKind::disable_observed}).empty(),
        "bulk disable requires the reviewed Runtime epoch");
    for (std::size_t length = 0; length < disconnect_bytes.size(); ++length)
        require(!app::decode_node_command_request({disconnect_bytes.begin(), disconnect_bytes.begin() + length}),
            "disconnect rejects every truncation");
    auto extra_disconnect = disconnect_bytes; extra_disconnect.push_back(0);
    require(!app::decode_node_command_request(extra_disconnect), "disconnect rejects trailing data");
    for (const auto seconds : {1U, 60U, 300U, 900U, 3600U}) {
        const auto drain = app::decode_lifecycle_command_request(app::encode_lifecycle_command_request(
            {app::AlphaLifecycleCommandKind::begin_drain, seconds}));
        require(drain && drain->timeout_seconds == seconds, "finite drain duration round trips");
    }
    require(app::decode_lifecycle_command_request(app::encode_lifecycle_command_request(
        {app::AlphaLifecycleCommandKind::cancel_drain, 0})).has_value(), "cancel drain round trips");
    require(app::encode_lifecycle_command_request({app::AlphaLifecycleCommandKind::begin_drain, 0}).empty() &&
        app::encode_lifecycle_command_request({app::AlphaLifecycleCommandKind::begin_drain, 3601}).empty() &&
        app::encode_lifecycle_command_request({app::AlphaLifecycleCommandKind::cancel_drain, 1}).empty(),
        "unbounded, expired and malformed drain requests refuse");
    const auto drain_bytes = app::encode_lifecycle_command_request({});
    for (std::size_t length = 0; length < drain_bytes.size(); ++length)
        require(!app::decode_lifecycle_command_request({drain_bytes.begin(), drain_bytes.begin() + length}),
            "drain rejects every truncation");
    const auto local_start_request = app::decode_local_session_start_request(
        app::encode_local_session_start_request({3}));
    require(local_start_request && local_start_request->node_number == 3,
        "exact-node local-session start request round trips");
    const auto local_start = app::decode_local_session_start_result(
        app::encode_local_session_start_result({
            {app::LocalControlResponseCode::ok, "local_session_reserved"},
            "127.0.0.1", 49152, local_identity, app::kAlphaLocalSessionTimeoutMilliseconds}));
    require(local_start && local_start->port == 49152 &&
        local_start->reservation == local_identity &&
        local_start->timeout_milliseconds == app::kAlphaLocalSessionTimeoutMilliseconds,
        "one-use loopback reservation result round trips with bounded expiry");
    const auto local_cancel = app::decode_local_session_cancel_request(
        app::encode_local_session_cancel_request(
            {local_identity, app::AlphaLocalSessionCancelReason::client_launch_failed}));
    require(local_cancel && local_cancel->reservation == local_identity &&
        local_cancel->reason == app::AlphaLocalSessionCancelReason::client_launch_failed,
        "typed local-session cancellation round trips");
    require(app::encode_local_session_start_request({0}).empty() &&
        app::encode_local_session_start_request({129}).empty(),
        "local-session request rejects nodes outside the fixed pool");
    auto malformed_local = app::encode_local_session_cancel_request(
        {local_identity, app::AlphaLocalSessionCancelReason::operator_cancelled});
    malformed_local.back() = 99;
    require(!app::decode_local_session_cancel_request(malformed_local),
        "unknown local-session cancellation reason fails closed");
    require(app::encode_local_session_start_result({
        {app::LocalControlResponseCode::ok, "bad_host"}, "0.0.0.0", 23,
        local_identity, app::kAlphaLocalSessionTimeoutMilliseconds}).empty(),
        "successful local-session result is restricted to exact loopback");
    const auto local_refusal = app::decode_local_session_start_result(
        app::encode_local_session_start_result({
            {app::LocalControlResponseCode::invalid_request, "node_not_ready"},
            {}, 0, {}, 0}));
    require(local_refusal && local_refusal->response.code == app::LocalControlResponseCode::invalid_request &&
        local_refusal->port == 0 && !local_refusal->reservation.valid(),
        "local-session refusal carries no reusable endpoint or lease capability");
    const auto settings = app::decode_settings_request(
        app::encode_settings_request({"Spitfire-X BBS", "127.0.0.1", 2323}));
    require(settings && settings->telnet_port == 2323 && settings->ssh_enabled &&
        settings->ssh_port == 2222 && settings->ssh_bind == "0.0.0.0" && settings->allow_new_users,
        "bounded settings request round trips");

    app::AlphaConfigurationSnapshot snapshot;
    snapshot.active = configuration();
    snapshot.saved = snapshot.active;
    snapshot.saved.configuration_revision = 2;
    snapshot.saved.logical_nodes = 3;
    snapshot.activation = app::AlphaConfigurationActivation::restart_required;
    snapshot.detail = "Restart required - logical node count changed";
    const auto snapshot_bytes = app::encode_configuration_snapshot(snapshot);
    const auto snapshot_decoded = app::decode_configuration_snapshot(snapshot_bytes);
    require(snapshot_decoded && snapshot_decoded->active.configuration_revision == 1 &&
        snapshot_decoded->saved.configuration_revision == 2 &&
        snapshot_decoded->saved.logical_nodes == 3 &&
        snapshot_decoded->activation == app::AlphaConfigurationActivation::restart_required &&
        snapshot_decoded->detail == snapshot.detail,
        "active and saved configuration revisions round trip with exact activation state");
    auto malformed_snapshot = snapshot_bytes;
    malformed_snapshot[malformed_snapshot.size() - snapshot.detail.size() - 3U] = 99;
    require(!app::decode_configuration_snapshot(malformed_snapshot),
        "unknown configuration activation state fails closed");

    app::AlphaConfigurationUpdateRequest configuration_update;
    configuration_update.expected_revision = 1;
    configuration_update.configuration = snapshot.saved;
    const auto update_bytes = app::encode_configuration_update_request(configuration_update);
    const auto update_decoded = app::decode_configuration_update_request(update_bytes);
    require(update_decoded && update_decoded->expected_revision == 1 &&
        update_decoded->configuration.configuration_revision == 2 &&
        update_decoded->configuration.logical_nodes == 3,
        "complete revision-checked configuration update round trips");
    configuration_update.expected_revision = 0;
    require(app::encode_configuration_update_request(configuration_update).empty(),
        "configuration update cannot omit its compare-and-swap revision");

    app::AlphaConfigurationUpdateResult update_result{
        app::LocalControlResponseCode::ok, "configuration_saved_restart_required",
        app::AlphaConfigurationActivation::restart_required, 1, 2,
        "Restart required - logical node count changed"};
    const auto result_decoded = app::decode_configuration_update_result(
        app::encode_configuration_update_result(update_result));
    require(result_decoded && result_decoded->code == app::LocalControlResponseCode::ok &&
        result_decoded->active_revision == 1 && result_decoded->saved_revision == 2 &&
        result_decoded->activation == app::AlphaConfigurationActivation::restart_required,
        "configuration result distinguishes durable saved and active revisions");

    const app::AlphaControlResponse response{app::LocalControlResponseCode::ok, "complete"};
    const auto response_decoded = app::decode_control_response(app::encode_control_response(response));
    require(response_decoded && response_decoded->code == app::LocalControlResponseCode::ok,
        "control response round trips");
}

void bootstrap_prerequisite_tests() {
    {
        app::AlphaRuntimeStatus stopped;stopped.runtime_epoch.bytes[0]=1;stopped.status_sequence=1;
        stopped.state=app::AlphaRuntimePresentationState::stopped;
        stopped.lifecycle.state=app::RuntimeState::stopped;
        stopped.lifecycle.owner_mode=app::RuntimeOwnershipMode::service;
        stopped.lifecycle.configured_nodes=1;stopped.nodes.resize(1);stopped.nodes[0].number=1;
        stopped.diagnostic_code="service_stopped";
        const auto bytes=app::encode_runtime_status(stopped);
        require(!bytes.empty(),"stopped service has a valid bounded status representation");
        const auto decoded=app::decode_runtime_status(bytes);
        require(decoded&&decoded->state==app::AlphaRuntimePresentationState::stopped&&
            decoded->lifecycle.owner_mode==app::RuntimeOwnershipMode::service&&
            decoded->lifecycle.state==app::RuntimeState::stopped,"matched protocol preserves explicit stopped service state");
        auto active=four_callers();active.lifecycle.owner_mode=app::RuntimeOwnershipMode::service;
        const auto running=app::decode_runtime_status(app::encode_runtime_status(active));
        require(running&&running->lifecycle.owner_mode==app::RuntimeOwnershipMode::service&&running->nodes.size()==4,
            "service running status retains the normal authoritative node model");
        active.lifecycle.owner_mode=static_cast<app::RuntimeOwnershipMode>(2);
        require(app::encode_runtime_status(active).empty(),"unknown runtime ownership is still refused");
    }
    app::AlphaRuntimeStatus status;
    status.runtime_epoch.bytes[0] = 1;
    status.status_sequence = 1;
    status.lifecycle.configured_nodes = 4;
    status.state = app::AlphaRuntimePresentationState::recovery_required;
    status.diagnostic_code = "apply Alpha Bin owner and read-execute DACL";
    auto blocked = app::bootstrap_prerequisite_failure(status);
    require(blocked && blocked->diagnostic_code == status.diagnostic_code,
        "provisioning diagnostic survives absent bootstrap policy instead of becoming bootstrap_request_invalid");
    const auto decoded = app::decode_runtime_status(app::encode_runtime_status(status));
    require(decoded && app::bootstrap_prerequisite_failure(*decoded)->diagnostic_code == status.diagnostic_code,
        "original diagnostic survives Runtime-to-Control-Panel status serialization");
    status.security_policy_revision = 1;
    for (const std::string& hash : {std::string{}, std::string(63, 'a'), std::string(64, 'A'), std::string(64, 'x')}) {
        status.security_policy_hash = hash;
        require(app::bootstrap_prerequisite_failure(status).has_value(), "missing or malformed policy blocks setup before secrets");
    }
    status.security_policy_hash = configuration().security_policy_hash;
    require(app::bootstrap_prerequisite_failure(status).has_value(),
        "recovery never opens first-run even with authoritative policy");
    require(app::alpha_recovery_guidance("configuration_missing").find("missing") != std::string_view::npos &&
        app::alpha_recovery_guidance("configuration_invalid").find("invalid") != std::string_view::npos &&
        app::alpha_recovery_guidance("sensitive-untrusted-diagnostic").find("sensitive") == std::string_view::npos,
        "fixed recovery guidance distinguishes missing/invalid without reflecting arbitrary input");
    status.state = app::AlphaRuntimePresentationState::setup_required;
    require(!app::bootstrap_prerequisite_failure(status), "clean provisioned setup permits bootstrap");
    status.state = app::AlphaRuntimePresentationState::faulted;
    require(app::bootstrap_prerequisite_failure(status).has_value(), "non-setup state never permits bootstrap");
    status.diagnostic_code.clear();
    require(app::bootstrap_prerequisite_failure(status)->diagnostic_code == "runtime_setup_prerequisites_unavailable",
        "missing diagnostic uses truthful prerequisite fallback, not caller validation error");
}

app::AlphaRuntimeStatus four_callers() {
    app::AlphaRuntimeStatus s;
    s.runtime_epoch.bytes[0] = 42;
    s.status_sequence = 1;
    s.lifecycle.configured_nodes = s.lifecycle.active_work.caller_sessions = 4;
    s.state = app::AlphaRuntimePresentationState::ready;
    s.lifecycle.state = app::RuntimeState::online;
    s.configuration_loaded = s.bootstrap_complete = true;
    s.telnet_configured = s.telnet_active = s.ssh_configured = s.ssh_active = true;
    s.bbs_name.assign(app::kBbsNameMaximum, 'B');
    s.bbs_time_zone.assign(app::kBbsTimeZoneKeyMaximum, 'T');
    s.security_policy_revision = INT64_MAX;
    s.security_policy_hash.assign(64, 'f');
    s.telnet_bind.assign(app::kTelnetBindMaximum, '1');
    s.ssh_bind = s.telnet_bind;
    s.telnet_port = s.ssh_port = UINT16_MAX;
    s.storage.engine_version.assign(64, 'E');
    s.storage.last_operation.assign(128, 'O');
    s.diagnostic_code.assign(128, 'D');
    for (std::uint16_t i = 1; i <= 4; ++i) {
        app::AlphaNodeStatus n;
        n.number = i; n.state = sfx::core::NodeState::online;
        n.available = n.enabled = n.screen_active = true;
        n.assignment_sequence = 8; n.screen.sequence = 10;
        n.screen_owner = {s.runtime_epoch, i, n.assignment_sequence};
        n.connection = "Telnet"; n.caller_handle.assign(30, static_cast<char>('A' + i));
        n.activity.assign(64, 'a'); n.details.assign(128, 'd'); n.source_ip.assign(64, '1');
        n.terminal_columns = n.terminal_rows = 4096;
        n.connected_seconds = UINT64_MAX;
        for (std::size_t cell = 0; cell < n.screen.cells.size(); ++cell)
            n.screen.cells[cell] = {static_cast<std::uint8_t>((cell + i) % 256),
                static_cast<std::uint8_t>(cell % 256)};
        s.nodes.push_back(n);
    }
    for (std::size_t i = 0; i < app::kControlPanelMaximumLogEntries; ++i)
        s.logs.push_back({i + 1, INT64_MAX, std::string(16, 'L'), std::string(32, 'C'), std::string(160, 'M')});
    s.statistics.successful_logons = s.statistics.caller_seconds = s.statistics.new_users =
        {true, UINT64_MAX, UINT64_MAX};
    return s;
}

void c2_settings_protocol_tests() {
    app::AlphaSettingsRequest request{"C2 pending board", "127.0.0.1", 2324, true, "127.0.0.2", 2223, false};
    for (const bool allow : {false, true}) {
        request.allow_new_users = allow;
        const auto bytes = app::encode_settings_request(request);
        const auto decoded = app::decode_settings_request(bytes);
        require(decoded && decoded->allow_new_users == allow &&
            app::encode_settings_request(*decoded) == bytes, "registration policy round trips explicitly, both values");
        auto invalid = bytes; invalid.pop_back();
        require(!app::decode_settings_request(invalid), "old settings payload without explicit hostname policy rejects");
        invalid = bytes; invalid.back() = 2;
        require(!app::decode_settings_request(invalid), "non-boolean hostname value rejects");
        invalid = bytes; invalid[invalid.size() - 2] = 2;
        require(!app::decode_settings_request(invalid), "non-boolean registration value rejects");
        invalid = bytes; invalid.push_back(0);
        require(!app::decode_settings_request(invalid), "trailing settings bytes reject");
    }
    auto status = four_callers();
    status.allow_new_users = false;
    status.listener_restart_required = true;
    status.pending_settings = request;
    const auto bytes = app::encode_runtime_status(status);
    const auto decoded = app::decode_runtime_status(bytes);
    require(decoded && !decoded->allow_new_users && decoded->pending_settings &&
        decoded->pending_settings->allow_new_users &&
        decoded->pending_settings->bbs_name == request.bbs_name &&
        app::encode_runtime_status(*decoded) == bytes,
        "four screens plus active-Off/pending-On reconstruct exactly without conflating policy");
    const auto settings = app::encode_settings_request(request);
    const auto found = std::search(bytes.begin(), bytes.end(), settings.begin(), settings.end());
    require(found != bytes.end(), "locate bounded pending snapshot independently");
    const auto offset = static_cast<std::size_t>(found - bytes.begin());
    for (const auto field : {offset - 6, offset - 5, offset - 4, offset - 3,
            offset + settings.size() - 2, offset + settings.size() - 1}) {
        auto bad = bytes; bad[field] = 2;
        require(!app::decode_runtime_status(bad), "invalid restart/active/pending policy booleans reject");
    }
    auto bad = bytes; bad[offset - 2] = 255; bad[offset - 1] = 255;
    require(!app::decode_runtime_status(bad), "oversized pending snapshot length rejects before allocation");
    for (std::size_t count = 0; count < settings.size(); ++count) {
        bad.assign(bytes.begin(), bytes.begin() + offset + count);
        require(!app::decode_runtime_status(bad), "every truncated pending snapshot rejects");
    }
    auto inconsistent = status; inconsistent.pending_settings.reset();
    require(app::encode_runtime_status(inconsistent).empty(), "restart flag cannot omit authoritative pending settings");
    inconsistent = status; inconsistent.listener_restart_required = false;
    require(app::encode_runtime_status(inconsistent).empty(), "pending settings cannot claim no restart is required");
    inconsistent = status; inconsistent.pending_settings->bbs_name = "raw\rinput";
    require(app::encode_runtime_status(inconsistent).empty(), "pending presentation text remains sanitized");
    status.pending_settings->bbs_name.assign(app::kBbsNameMaximum, 'P');
    status.pending_settings->telnet_bind.assign(app::kTelnetBindMaximum, '1');
    status.pending_settings->ssh_bind.assign(app::kTelnetBindMaximum, '2');
    const auto maximum = app::encode_runtime_status(status);
    require(!maximum.empty() && maximum.size() <= app::kLocalControlMaximumPayloadBytes &&
        app::decode_runtime_status(maximum).has_value(), "maximum settings and four complete screens fit unchanged frame bound");
    std::cout << "PASS C2 settings/status: explicit policy, bounded pending snapshot, malformed rejection; maximum payload="
        << maximum.size() << " bytes\n";
}

void stage4_protocol_tests() {
    auto s = four_callers();
    for (auto& node : s.nodes) {
        node.caller_connected = true;
        node.account_id=4294967297LL+node.number;
        node.ssh_encrypted = node.connection == "SSH";
        node.receive_activity = 3; node.send_activity = 7;
    }
    const auto bytes = app::encode_runtime_status(s);
    require(!bytes.empty(), "maximum valid four-screen status encodes");
    const auto decoded = app::decode_runtime_status(bytes);
    require(decoded && app::encode_runtime_status(*decoded) == bytes,
        "maximum content round trips byte-exact, including four screen owners");
    require(decoded && decoded->nodes[0].caller_connected &&
        decoded->nodes[0].account_id==s.nodes[0].account_id &&
        decoded->nodes[0].receive_activity == 3 && decoded->nodes[0].send_activity == 7,
        "transport state and separate payload sequences round trip");
    std::cout << "maximum legacy-log four-screen status payload=" << bytes.size() << " bytes; limit=65536\n";
    app::LocalControlFrame frame;
    frame.request_id = 1; frame.payload.assign(app::kLocalControlMaximumPayloadBytes, 0x5a);
    auto wire = app::encode_local_control_frame(frame);
    require(wire.size() == 65556 && app::decode_local_control_frame(wire).frame.has_value(),
        "maximum valid frame is exactly 65536 payload plus 20 header bytes");
    wire[16] = 1; wire[17] = 128;
    require(app::decode_local_control_frame(wire).code == app::LocalControlFrameDecodeCode::oversized,
        "oversized declared frame rejects before parsing");
    auto oversized = bytes; oversized.resize(app::kLocalControlMaximumPayloadBytes + 1);
    require(!app::decode_runtime_status(oversized), "direct oversized status rejects");
    for (std::size_t end = 0; end < bytes.size(); end += 97) {
        const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + end);
        require(!app::decode_runtime_status(truncated), "truncated variable fields and screens reject");
    }
    auto trailing = bytes; trailing.push_back(0);
    require(!app::decode_runtime_status(trailing), "trailing status content rejects");

    // Locate wire fields independently of the production reader.
    std::size_t offset = 35;
    const auto skip_text = [&] {
        const auto length = static_cast<std::size_t>(bytes[offset]) + 256U * bytes[offset + 1];
        offset += length + 2;
    };
    skip_text(); skip_text(); offset += 8; skip_text();
    const auto count_offset = offset; offset += 2;
    std::array<std::size_t, 4> node_offsets{}, owner_offsets{};
    for (unsigned i = 0; i < 4; ++i) {
        node_offsets[i] = offset; offset += 5;
        for (unsigned j = 0; j < 6; ++j) skip_text();
        // Connected time, assignment, Door ID/launch, authenticated account,
        // transport flags, RX/TX, dimensions, screen flags and sequence.
        offset += 8 + 8 + 2 + 8 + 8 + 2 + 8 + 8 + 2 + 2 + 2 + 8;
        owner_offsets[i] = offset;
        offset += 26 + 4000;
    }
    for (unsigned count : {0U, 1U, 3U, 5U, 255U}) {
        auto bad = bytes; bad[count_offset] = static_cast<std::uint8_t>(count);
        require(!app::decode_runtime_status(bad), "malformed node count rejects");
    }
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned number : {0U, 5U, 128U, 255U}) {
            auto bad = bytes; bad[node_offsets[i]] = static_cast<std::uint8_t>(number);
            require(!app::decode_runtime_status(bad), "out-of-range node number rejects");
        }
        auto bad = bytes; bad[node_offsets[i]] = static_cast<std::uint8_t>((i + 1) % 4 + 1);
        require(!app::decode_runtime_status(bad), "duplicate or misordered node rejects");
        for (auto field : {node_offsets[i] + 2, node_offsets[i] + 3, node_offsets[i] + 4}) {
            bad = bytes; bad[field] = 255;
            require(!app::decode_runtime_status(bad), "invalid enum and booleans reject");
        }
        for (auto field : {owner_offsets[i], owner_offsets[i] + 16, owner_offsets[i] + 18}) {
            bad = bytes; ++bad[field];
            require(!app::decode_runtime_status(bad), "mismatched screen epoch, node or generation rejects");
        }
    }
    const auto reject = [&](auto mutate) {
        auto bad = s; mutate(bad);
        require(app::encode_runtime_status(bad).empty(), "inconsistent producer state fails closed");
    };
    reject([](auto& v) { v.runtime_epoch = {}; });
    reject([](auto& v) { v.status_sequence = 0; });
    reject([](auto& v) { v.lifecycle.active_work.caller_sessions = 3; });
    reject([](auto& v) { v.lifecycle.configured_nodes = 5; });
    reject([](auto& v) { v.nodes[1].caller_handle += 'x'; });
    reject([](auto& v) { v.nodes[2].activity = "raw\rinput"; });
    reject([](auto& v) { v.nodes[3].screen_active = false; });
    reject([](auto& v) { v.nodes[0].terminal_rows = 0; });
    reject([](auto& v) { v.nodes[0].terminal_columns = 4097; });
    reject([](auto& v) { v.nodes[0].connection = "other"; });
    reject([](auto& v) { v.nodes[0].connection = "Telnet"; v.nodes[0].ssh_encrypted = true; });
    reject([](auto& v) { v.nodes[0].caller_connected = false; v.nodes[0].ssh_encrypted = true; });
    reject([](auto& v) { v.logs[1].sequence = v.logs[0].sequence; });
    reject([](auto& v) { v.logs[0].message += 'x'; });

    auto invalid_configuration = s;
    invalid_configuration.lifecycle.configured_nodes = 5;
    invalid_configuration.state = app::AlphaRuntimePresentationState::recovery_required;
    invalid_configuration.diagnostic_code = "alpha_node_count_requires_1_to_4";
    invalid_configuration.nodes.clear();
    invalid_configuration.lifecycle.active_work.caller_sessions = 0;
    invalid_configuration.bootstrap_complete = invalid_configuration.telnet_active = invalid_configuration.ssh_active = false;
    const auto recovery = app::decode_runtime_status(app::encode_runtime_status(invalid_configuration));
    require(recovery && recovery->lifecycle.configured_nodes == 5 && recovery->nodes.empty(),
        "rejected configuration count is reported truthfully without clamping or activating node five");
    invalid_configuration.telnet_active = true;
    require(app::encode_runtime_status(invalid_configuration).empty(),
        "configuration-recovery exception cannot carry an active listener or caller");

    app::AlphaStatusTracker tracker;
    require(!tracker.accept(s), "no status authority before authenticated hello");
    require(!tracker.bind_authenticated_epoch({}), "zero hello epoch refuses");
    require(tracker.bind_authenticated_epoch(s.runtime_epoch) && tracker.accept(s), "hello pins epoch");
    require(!tracker.accept(s), "duplicate status sequence rejects");
    ++s.status_sequence;
    s.nodes[0].receive_activity = 5; s.nodes[0].send_activity = 9;
    require(tracker.accept(s), "new payload counters advance within the assignment");
    auto stale_traffic = s; ++stale_traffic.status_sequence;
    --stale_traffic.nodes[0].receive_activity;
    require(!tracker.accept(stale_traffic), "receive counter regression refuses atomically");
    stale_traffic = s; ++stale_traffic.status_sequence;
    --stale_traffic.nodes[0].send_activity;
    require(!tracker.accept(stale_traffic), "send counter regression refuses atomically");
    auto old = s;
    ++s.status_sequence;
    auto bad = s; --bad.nodes[3].assignment_sequence; --bad.nodes[3].screen_owner.generation;
    require(!tracker.accept(bad) && tracker.accept(s), "stale generation rejects atomically without advancing other nodes");
    ++s.status_sequence; bad = s; --bad.nodes[2].screen.sequence;
    require(!tracker.accept(bad) && tracker.accept(s), "delayed older screen rejects despite newer frame sequence");
    ++s.status_sequence;
    auto& released = s.nodes[1];
    released = {}; released.number = 2; released.assignment_sequence = 8;
    released.available = released.enabled = true; released.state = sfx::core::NodeState::ready;
    --s.lifecycle.active_work.caller_sessions;
    require(tracker.accept(s), "release clears screen and caller within same generation");
    bad = old; bad.status_sequence = s.status_sequence + 1;
    require(!tracker.accept(bad), "released generation cannot resurrect a caller or screen");
    ++s.status_sequence; released = old.nodes[1];
    ++released.assignment_sequence; ++released.screen_owner.generation;
    released.screen.sequence = 1; ++s.lifecycle.active_work.caller_sessions;
    require(tracker.accept(s), "immediate reuse requires increasing generation and fresh screen owner");
    auto restarted = old; restarted.runtime_epoch.bytes[0] = 43;
    for (auto& n : restarted.nodes) n.screen_owner.runtime_epoch = restarted.runtime_epoch;
    require(!tracker.accept(restarted), "new epoch cannot arrive through old authenticated connection");
    require(tracker.bind_authenticated_epoch(restarted.runtime_epoch) && tracker.accept(restarted),
        "new authenticated Runtime accepts reset sequences and generations");
    old.status_sequence = UINT64_MAX;
    require(!tracker.accept(old), "old epoch never overwrites new Runtime even with largest sequence");

    app::AlphaNodeIndicators indicators;
    for (auto& node : restarted.nodes) node.caller_connected = true;
    indicators.observe(restarted, 1000);
    for (std::uint16_t i = 1; i <= 4; ++i) require(indicators.bold(i, 1000), "four simultaneous assignment indicators");
    for (std::uint16_t i = 1; i <= 4; ++i) require(!indicators.bold(i, 1250), "independent pulse off phase");
    ++restarted.nodes[1].screen.sequence;
    indicators.observe(restarted, 1300);
    require(!indicators.bold(2, 1300) && !indicators.bold(1, 1300) && !indicators.bold(3, 1300) &&
        !indicators.bold(4, 1300), "screen activity cannot restart any connection indicator");
    indicators.clear();
    for (std::uint16_t i = 1; i <= 4; ++i) require(!indicators.bold(i, 1300), "presentation reset removes every indicator");
    ++restarted.status_sequence;
    require(tracker.accept(restarted), "fresh authoritative status after clearing presentation keeps ordering protection");
    indicators.observe(restarted, 1500);
    for (std::uint16_t i = 1; i <= 4; ++i) require(indicators.bold(i, 1500), "fresh status reconstructs four indicators");
    require(!tracker.accept(old), "presentation reconstruction never erases epoch high-water mark");
}

void scheduler_protocol_tests() {
    app::AlphaSchedulerSettingsRequest settings;
    settings.expected_revision = 7;
    settings.enabled = false;
    settings.local_hour = 23;
    settings.local_minute = 59;
    const auto settings_bytes = app::encode_scheduler_settings_request(settings);
    const auto decoded_settings = app::decode_scheduler_settings_request(settings_bytes);
    require(decoded_settings && decoded_settings->job_id == app::kBackupJobId &&
                decoded_settings->expected_revision == 7 && !decoded_settings->enabled &&
                decoded_settings->local_hour == 23 && decoded_settings->local_minute == 59 &&
                !decoded_settings->backup_destination,
            "typed scheduler settings round trip with optimistic revision");
    auto malformed = settings_bytes;
    malformed[malformed.size() - 2] = 60;
    require(!app::decode_scheduler_settings_request(malformed),
            "invalid scheduler minute rejects");
    malformed = settings_bytes;
    malformed.back() = 2;
    require(!app::decode_scheduler_settings_request(malformed),
            "unknown destination-presence tag rejects");
    malformed = settings_bytes;
    malformed.pop_back();
    require(!app::decode_scheduler_settings_request(malformed),
            "old scheduler layout rejects in matched protocol version 30");
    settings.backup_destination = u8"C:\\Private backups\\Ã§Å’Â«";
    const auto live_bytes = app::encode_scheduler_settings_request(settings);
    const auto live = app::decode_scheduler_settings_request(live_bytes);
    require(live && live->backup_destination == settings.backup_destination,
            "live Unicode backup destination round trips without retention mutation");
    settings.backup_destination = std::string{};
    const auto defaults = app::decode_scheduler_settings_request(app::encode_scheduler_settings_request(settings));
    require(defaults && defaults->backup_destination && defaults->backup_destination->empty(),
            "explicit empty destination selects default and differs from absent");
    for (const auto* invalid : {"relative", "\\\\server\\share", "C:\\bad\\..", "C:\\bad\\NUL", "C:\\bad\\"}) {
        settings.backup_destination = invalid;
        require(app::encode_scheduler_settings_request(settings).empty(),
                "unsafe destination syntax rejected before transport");
    }
    malformed = live_bytes; malformed.pop_back();
    require(!app::decode_scheduler_settings_request(malformed), "truncated destination rejects");
    malformed = live_bytes; malformed.push_back(0);
    require(!app::decode_scheduler_settings_request(malformed), "trailing destination input rejects");
    malformed = live_bytes;
    // A remote frame with a syntactically unsafe path is independently refused.
    malformed[malformed.size() - live->backup_destination->size() + 1] = '/';
    require(!app::decode_scheduler_settings_request(malformed), "unsafe destination bytes independently rejected by decoder");
    settings.backup_destination = std::string(2049, 'x');
    require(app::encode_scheduler_settings_request(settings).empty(), "overbound destination rejects");
    const auto run_bytes = app::encode_scheduler_run_request({});
    require(app::decode_scheduler_run_request(run_bytes).has_value(),
            "typed built-in backup Run Now request round trip");
    malformed = run_bytes;
    malformed.push_back(0);
    require(!app::decode_scheduler_run_request(malformed),
            "trailing Run Now input rejects");

    auto status = four_callers();
    status.scheduler.configuration_loaded = true;
    status.scheduler.enabled = true;
    status.scheduler.configuration_revision = 7;
    status.scheduler.local_hour = 3;
    status.scheduler.local_minute = 15;
    status.scheduler.next_run_utc = 1'800'000'000;
    status.scheduler.last_run_utc = 1'700'000'000;
    status.scheduler.last_result = app::ScheduledJobResult::success;
    status.scheduler.last_detail = u8"C:\\SFX\\Backup\\scheduled-Ã§Å’Â«.zip";
    for (std::size_t index = 0; index < 32; ++index) {
        const auto started = 1'699'990'000 + static_cast<std::int64_t>(index * 100);
        status.scheduler.history.push_back({
            "0123456789ABCDEF0123456789ABCDE" + std::to_string(index % 10),
            started, started + 7, app::ScheduledJobResult::success,
            u8"C:\\SFX\\Backup\\scheduled-Ã§Å’Â«.zip"});
    }
    status.scheduler.diagnostic_code = "scheduler_ready";
    const auto status_bytes = app::encode_runtime_status(status);
    const auto decoded_status = app::decode_runtime_status(status_bytes);
    require(decoded_status && decoded_status->scheduler.configuration_loaded &&
                decoded_status->scheduler.configuration_revision == 7 &&
                decoded_status->scheduler.last_detail == status.scheduler.last_detail &&
                decoded_status->scheduler.history.size() == 32 &&
                decoded_status->scheduler.history.front().detail ==
                    status.scheduler.history.front().detail &&
                decoded_status->scheduler.history.back().run_id ==
                    status.scheduler.history.back().run_id &&
                app::encode_runtime_status(*decoded_status) == status_bytes,
            "scheduler configuration, timing, result, Unicode path and history status round trip");
    auto invalid_history = status;
    invalid_history.scheduler.history.push_back(status.scheduler.history.back());
    require(app::encode_runtime_status(invalid_history).empty(),
            "more than 32 scheduler history entries never reach a client");
    invalid_history = status;
    invalid_history.scheduler.history.front().detail.assign(
        app::kSchedulerStatusHistoryDetailMaximum + 1, 'x');
    require(app::encode_runtime_status(invalid_history).empty(),
            "oversized scheduler history detail never reaches a client");
    const auto long_unicode = std::string(124, 'x') + u8"Ã§Å’Â«-tail";
    const auto bounded = app::bounded_scheduler_status_history_detail(long_unicode);
    require(bounded.size() <= app::kSchedulerStatusHistoryDetailMaximum &&
                bounded.substr(bounded.size() - 3) == "..." &&
                bounded.find('\0') == std::string::npos,
            "history detail is UTF-8-boundary-truncated with an explicit marker");
    status.scheduler.active = true;
    status.scheduler.active_scheduled = true;
    status.scheduler.queued = true;
    status.scheduler.active_run_id = "0123456789ABCDEF0123456789ABCDEF";
    status.scheduler.active_started_utc = 1'700'000'100;
    status.scheduler.last_result = app::ScheduledJobResult::running;
    const auto active_status = app::decode_runtime_status(
        app::encode_runtime_status(status));
    require(active_status && active_status->scheduler.queued,
            "queued active scheduler run has one stable wire identity");
    status.scheduler.active_run_id = "invalid";
    require(app::encode_runtime_status(status).empty(),
            "malformed active scheduler identity never reaches Control Panel");
    status.scheduler.active = false;
    status.scheduler.queued = true;
    status.scheduler.active_run_id.clear();
    status.scheduler.active_started_utc = 0;
    status.scheduler.active_scheduled = false;
    status.scheduler.last_result = app::ScheduledJobResult::success;
    require(app::encode_runtime_status(status).empty(),
            "queued status without one durable active run is refused");
}

void l1_typed_status_tests() {
    auto s = four_callers(); s.logs.clear();
    for (std::size_t i = 0; i < 32; ++i) {
        app::OperationalContext c;
        c.epoch.bytes.fill(255); // historical event origin is NOT node/status authority
        c.origin_sequence = UINT64_MAX - 32 + i;
        c.node = 4; c.generation = c.attempt = c.duration_seconds = UINT64_MAX;
        c.transport = app::OperationalTransport::ssh; c.source_ip = "255.255.255.255";
        c.peer_port = c.local_port = 65535; c.columns = c.rows = 4096;
        c.account_id = c.committed_account_id = c.verified_account_id = std::numeric_limits<app::AccountId>::max();
        c.caller.assign(30, '|'); c.correlation_id.bytes.fill(255); c.durable_session_id.bytes.fill(255);
        c.resource_id = UINT64_MAX; c.resource_name.assign(128, '%'); c.diagnostic_code.assign(96, 'z');
        c.hostname = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
            std::string(63, 'c') + "." + std::string(61, 'd');
        s.logs.push_back(app::make_operational_entry(i + 1, INT64_MAX, {app::OperationalKind::stage_changed, {}, c}));
    }
    s.log_health = 31;
    s.scheduler.configuration_loaded = true;
    s.scheduler.last_detail.assign(2048, 'd');
    for (std::size_t index = 0; index < app::kSchedulerHistoryLimit; ++index) {
        const auto started = 1'699'000'000 + static_cast<std::int64_t>(index * 100);
        s.scheduler.history.push_back({
            "FEDCBA9876543210FEDCBA987654321" + std::to_string(index % 10),
            started, started + 99, app::ScheduledJobResult::failed,
            std::string(app::kSchedulerStatusHistoryDetailMaximum, 'h')});
    }
    for (auto& node : s.nodes) {
        node.source_ip = "255.255.255.255";
        node.hostname = s.logs.front().event->context.hostname;
    }
    s.hostname_exhausted = true;
    const auto bytes = app::encode_runtime_status(s);
    const auto decoded = app::decode_runtime_status(bytes);
    require(decoded && decoded->logs.size() == 32 && decoded->log_health == 31 &&
        decoded->scheduler.history.size() == app::kSchedulerHistoryLimit &&
        app::encode_runtime_status(*decoded) == bytes && bytes.size() < 65536,
        "four complete screens plus 32 worst typed events and scheduler history round trip exactly");
    std::cout << "CR3 maximum typed four-screen/32-log/32-job-history status=" <<
        bytes.size() << " bytes; limit=65536\n";
    auto bad = s; bad.log_health = 32;
    require(app::encode_runtime_status(bad).empty(), "invalid health bits reject");
    bad = s; bad.logs[1].event->context.origin_sequence = bad.logs[0].event->context.origin_sequence;
    require(app::encode_runtime_status(bad).empty(), "duplicate typed origin rejects despite different delivery sequence");
    bad = s; bad.logs[1].event->context.origin_sequence = 1;
    require(app::encode_runtime_status(bad).empty(), "out-of-order same-epoch origin rejects");
    bad = s; bad.logs[1].event->context.epoch.bytes[0] = 1;
    require(!app::encode_runtime_status(bad).empty(), "different historical origin remains data, not current node authority");
    auto tracker = app::AlphaStatusTracker{};
    require(tracker.bind_authenticated_epoch(s.runtime_epoch) && tracker.accept(s), "historic events do not alter authenticated epoch binding");
    bad = s; bad.runtime_epoch = bad.logs.front().event->context.epoch; ++bad.status_sequence;
    require(!tracker.accept(bad), "historic event epoch can never replace authenticated Runtime epoch");
    const auto first_record = app::encode_operational_record(s.logs.front());
    const auto found = std::search(bytes.begin(), bytes.end(), first_record.begin(), first_record.end());
    require(found != bytes.end(), "independent locator finds typed wire record");
    const auto offset = static_cast<std::size_t>(found - bytes.begin());
    auto invalid = bytes; invalid[offset - 2] = 1; invalid[offset - 1] = 5;
    require(!app::decode_runtime_status(invalid), "1281-byte record length rejects before copy");
    invalid = bytes; invalid[offset] = 'X';
    require(!app::decode_runtime_status(invalid), "unknown record version rejects");
    invalid = bytes; invalid[offset + 8] = '\x1b';
    require(!app::decode_runtime_status(invalid), "embedded control input rejects");
}

void l2_hostname_status_tests() {
    auto status = four_callers(); status.logs.clear();
    for (auto& n : status.nodes) {
        n.source_ip = "127.0.0.1"; n.hostname = "node" + std::to_string(n.number) + ".example";
    }
    status.hostname_lookup = true; status.hostname_exhausted = true;
    status.pending_settings = app::AlphaSettingsRequest{"Host test", "127.0.0.1", 2323, true, "127.0.0.1", 2222, true, false};
    status.listener_restart_required = true;
    const auto encoded = app::encode_runtime_status(status);
    const auto decoded = app::decode_runtime_status(encoded);
    require(decoded && decoded->hostname_lookup && decoded->hostname_exhausted &&
        !decoded->pending_settings->hostname_lookup && app::encode_runtime_status(*decoded) == encoded,
        "four hostname hints, active On/pending Off and permanent degradation round trip");
    for (const auto* invalid : {"bad|name", "bad\nname", "bad\x1b[31m", "MixedCase", "root.example.", "-bad", "a..b"}) {
        auto malformed = status; malformed.nodes[1].hostname = invalid;
        require(app::encode_runtime_status(malformed).empty(), "invalid hostname rejected before protocol publication");
    }
    auto malformed = status; malformed.nodes[0].hostname.assign(254, 'x');
    require(app::encode_runtime_status(malformed).empty(), "oversized hostname rejected");
    malformed = status; malformed.hostname_lookup = false;
    require(app::encode_runtime_status(malformed).empty(), "Off cannot carry cached live hostnames");
    malformed = status; malformed.nodes[0].source_ip = "not-an-ip";
    require(app::encode_runtime_status(malformed).empty(), "hostname must accompany canonical numeric IP");
    app::AlphaStatusTracker tracker;
    require(tracker.bind_authenticated_epoch(status.runtime_epoch) && tracker.accept(status), "hostname status authenticated epoch");
    auto stale = status; ++status.status_sequence; ++status.nodes[0].assignment_sequence;
    ++status.nodes[0].screen_owner.generation; status.nodes[0].hostname = "replacement.example";
    require(tracker.accept(status) && !tracker.accept(stale), "delayed hostname snapshot cannot overwrite reused assignment");
    app::AlphaNodeIndicators indicators; indicators.observe(status, 0);
    for (std::uint16_t i = 1; i <= 4; ++i) require(!indicators.bold(i, 250), "all assignment pulses settle");
    ++status.status_sequence; status.nodes[0].hostname = "metadata-only.example";
    indicators.observe(status, 300);
    for (std::uint16_t i = 1; i <= 4; ++i) require(!indicators.bold(i, 300), "hostname-only metadata never starts activity animation");
}
void high_node_status_transfer_tests() {
    auto status = four_callers();
    status.nodes.resize(1);
    const auto prototype = status.nodes.front();
    status.nodes.clear();
    status.lifecycle.configured_nodes = status.lifecycle.active_work.caller_sessions = 128;
    const auto maximum_hostname = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
        std::string(63, 'c') + "." + std::string(61, 'd');
    for (std::uint16_t number = 1; number <= 128; ++number) {
        auto node = prototype;
        node.number = number; node.screen_owner.node = number;
        node.hostname = maximum_hostname; node.source_ip = "127.0.0." + std::to_string(number);
        status.nodes.push_back(std::move(node));
    }
    status.reporting.available = status.administration_history.available = true;
    status.reporting.counts.fill(UINT64_MAX);
    status.logs.clear();
    for (unsigned i=1;i<=32;++i) {
        app::OperationalContext context;
        context.epoch=status.runtime_epoch; context.origin_sequence=i+100;
        context.node=128; context.generation=context.attempt=UINT64_MAX;
        context.account_id=INT64_MAX; context.caller.assign(30,'|');
        context.transport=app::OperationalTransport::ssh; context.source_ip="255.255.255.255";
        context.hostname=maximum_hostname; context.correlation_id.bytes.fill(255);
        context.durable_session_id.bytes.fill(255); context.resource_id=1; context.resource_name.assign(128,'%');
        context.diagnostic_code.assign(96,'d'); context.duration_seconds=UINT64_MAX;
        auto event=app::make_operational_entry(i,1'700'000'000,{app::OperationalKind::connection_ended,{},context});
        status.reporting.recent.push_back(event);
        status.logs.push_back(event);
        app::Identifier128 id; id.bytes[0]=static_cast<std::uint8_t>(i);
        status.administration_history.recent.push_back({true,8,INT64_MAX,INT64_MAX,
            1'700'000'000-static_cast<std::int64_t>(i),id});
    }
    status.scheduler.configuration_loaded=true;
    status.scheduler.last_detail.assign(2048,'d');
    for (unsigned i=0;i<32;++i) status.scheduler.history.push_back({
        "FEDCBA9876543210FEDCBA987654321"+std::to_string(i%10),1'699'000'000+i*100,
        1'699'000'099+i*100,app::ScheduledJobResult::failed,
        std::string(app::kSchedulerStatusHistoryDetailMaximum,'h')});
    require(app::valid_alpha_runtime_status(status), "all 128 caller metadata/identity tuples validate");
    require(app::encode_runtime_status(status).empty(), "all 128 terminal canvases cannot enter a snapshot transfer");
    app::AlphaStatusSnapshotCache cache;
    require(cache.capture(status, 128), "bounded transfer captures one selected high-node screen");
    unsigned chunks = 0;
    const auto assembled = app::read_status_snapshot(128, [&](const app::AlphaStatusRequest& request)
        -> std::optional<std::vector<std::uint8_t>> {
        const auto encoded_request = app::encode_status_request(request);
        require(app::decode_status_request(encoded_request).has_value(), "status cursor round trips");
        auto payload = cache.chunk(request);
        require(!payload.empty() && payload.size() <= app::kLocalControlMaximumPayloadBytes,
            "every status chunk stays within locked 64-KiB frame ceiling");
        app::LocalControlFrame frame;
        frame.type = app::LocalControlMessageType::status_response; frame.request_id = ++chunks;
        frame.payload = payload;
        require(app::decode_local_control_frame(app::encode_local_control_frame(frame)).frame.has_value(),
            "each high-node chunk independently fits a normal authenticated frame");
        return payload;
    });
    require(assembled && chunks > 1 && chunks <= 5 && assembled->nodes.size() == 128 &&
        assembled->nodes[127].screen_included && assembled->nodes[127].screen.cells[1].character ==
            prototype.screen.cells[1].character, "bounded multi-frame transfer reconstructs complete 128-node snapshot");
    require(assembled->reporting.recent.size()==32 && assembled->administration_history.recent.size()==32 &&
        assembled->reporting.counts[0]==UINT64_MAX,"maximum reporting and persisted audit histories survive 128-node transfer");
    for (std::size_t index = 0; index < 127; ++index)
        require(!assembled->nodes[index].screen_included && assembled->nodes[index].screen_active &&
            assembled->nodes[index].screen.sequence == prototype.screen.sequence,
            "omitted canvases retain live identity and sequence without blank-screen authority");
    auto changed = status; ++changed.status_sequence;
    app::AlphaStatusSnapshotCache different;
    require(different.capture(changed, 128), "second snapshot captures independently");
    require(!app::read_status_snapshot(128, [&](const app::AlphaStatusRequest& query)
        -> std::optional<std::vector<std::uint8_t>> {
        if (query.offset == 0) return cache.chunk(query);
        auto substituted = query; substituted.snapshot_sequence = changed.status_sequence;
        return different.chunk(substituted);
    }), "mixed snapshot chunks cannot fabricate a complete status");
    for (const auto field : {0U, 8U, 12U}) {
        require(!app::read_status_snapshot(128, [&](const app::AlphaStatusRequest& query)
            -> std::optional<std::vector<std::uint8_t>> {
            auto payload = cache.chunk(query);
            if (!payload.empty()) payload[field] ^= 0x80;
            return payload;
        }), "changed sequence/offset/total rejects without partial status publication");
    }
    require(!app::read_status_snapshot(127, [&](const app::AlphaStatusRequest& query)
        -> std::optional<std::vector<std::uint8_t>> { return cache.chunk(query); }),
        "different selected-node request cannot reuse cached cells");
    const auto initial = app::encode_status_request({128, 0, 0});
    for (std::size_t end = 0; end < initial.size(); ++end)
        require(!app::decode_status_request({initial.begin(), initial.begin() + end}), "truncated status cursor refuses");
    require(app::encode_status_request({129,0,0}).empty() && app::encode_status_request({1,0,1}).empty() &&
        app::encode_status_request({1,1,0}).empty(), "invalid selected-node or continuation identity refuses");
    app::AlphaStatusTracker tracker;
    auto four = four_callers();
    require(tracker.bind_authenticated_epoch(four.runtime_epoch) && tracker.accept(four), "tracker begins with default capacity");
    auto growing = *assembled; ++growing.status_sequence;
    require(!tracker.accept(growing), "node-count change without a newer active configuration is refused");
    ++growing.configuration_revision;
    require(tracker.accept(growing), "new active configuration permits real capacity expansion under the same Runtime epoch");
    std::cout << "PASS 128-node immutable status transfer: " << chunks << " bounded frames, selected canvas only\n";
}
void reporting_protocol_tests() {
    app::AlphaRuntimeStatus status;
    status.lifecycle.configured_nodes=4;
    status.runtime_epoch.bytes[0]=1; status.status_sequence=1;
    status.reporting.available=status.administration_history.available=true;
    app::OperationalContext context; context.epoch=status.runtime_epoch; context.origin_sequence=9;
    status.reporting.recent.push_back(app::make_operational_entry(2,100,
        {app::OperationalKind::throttle_cleared,{},context}));
    status.reporting.counts[6]=1;
    status.administration_history.recent.push_back({true,8,0,0,100,context.epoch});
    const auto bytes=app::encode_runtime_status(status);
    require_strict_message(bytes,app::decode_runtime_status);
    auto bad=status; bad.reporting.available=false;
    require(app::encode_runtime_status(bad).empty(),"unavailable operating counters cannot claim observations");
    bad=status; bad.reporting.recent[0].event->context.epoch.bytes[0]=2;
    require(app::encode_runtime_status(bad).empty(),"Runtime-lifetime events cannot import previous epochs");
    bad=status; bad.reporting.recent.push_back(bad.reporting.recent.front());
    require(app::encode_runtime_status(bad).empty(),"operating event sequence cannot duplicate or regress");
    bad=status; bad.administration_history.available=false;
    require(app::encode_runtime_status(bad).empty(),"unavailable persisted history cannot show stale records");
    bad=status; bad.administration_history.recent[0].kind=255;
    require(app::encode_runtime_status(bad).empty(),"unknown persisted audit kinds fail closed");
    auto malformed=bytes; malformed[malformed.size()-41]=255;
    require(!app::decode_runtime_status(malformed),"wire unknown audit kind rejected independently");
    malformed=bytes; malformed[malformed.size()-42]=2;
    require(!app::decode_runtime_status(malformed),"wire invalid audit domain boolean rejects");
    bad=status; bad.administration_history.recent[0].occurred_utc_seconds=-1;
    const auto negative=app::decode_runtime_status(app::encode_runtime_status(bad));
    require(negative && negative->administration_history.recent[0].occurred_utc_seconds==-1,
        "valid pre-Unix-epoch UTC remains signed rather than overflowing");
}
void message_configuration_protocol_tests(){
    app::LocalMessageConfigurationQuery q;
    auto bytes=app::encode_message_configuration_query(q);
    require(app::decode_message_configuration_query(bytes).has_value(),"message catalog default cursor roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_configuration_query({bytes.begin(),bytes.begin()+n}),"message query truncation rejected");
    q.group=app::MessageGroupId{1};q.cursor={2,0,7,100};
    require(app::decode_message_configuration_query(app::encode_message_configuration_query(q))->group->value==1,"echo query group remains stable identity");
    q.cursor.page_size=101;require(app::encode_message_configuration_query(q).empty(),"message page bound enforced");
    app::MessageConfigurationChange c;c.operation.bytes[0]=8;c.expected_configuration_revision=1;
    for(const auto action:{app::MessageConfigurationAction::group,app::MessageConfigurationAction::echo,app::MessageConfigurationAction::clone_permissions,app::MessageConfigurationAction::system_sysop,app::MessageConfigurationAction::line_limit}){
        c.action=action;c.group.name="Local";c.echo.name="Public";c.echo.group={1};c.clone_source={1};c.source_revision=1;c.clone_targets={{{2},1}};c.permission_mask=5;
        c.sysop=3;c.expected_account_revision=2;c.expected_assignment_revision=1;c.security_level=999;c.maximum_lines=1000;c.expected_limit_revision=1;
        bytes=app::encode_message_configuration_change(c);const auto decoded=app::decode_message_configuration_change(bytes);
        require(decoded && app::encode_message_configuration_change(*decoded)==bytes,"message action canonical roundtrip carries no authority flag");
        for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_configuration_change({bytes.begin(),bytes.begin()+n}),"message change truncation rejected");
        auto appended=bytes;appended.push_back(1);require(!app::decode_message_configuration_change(appended),"message change extra authority-like byte rejected");
        bytes[16]=255;require(!app::decode_message_configuration_change(bytes),"unknown message action rejected");
    }
    c.action=app::MessageConfigurationAction::clone_permissions;c.clone_targets.push_back({{2},1});require(app::encode_message_configuration_change(c).empty(),"duplicate clone target rejected before IPC");
    c.action=app::MessageConfigurationAction::line_limit;c.maximum_lines=0;require(app::encode_message_configuration_change(c).empty(),"no unlimited message line sentinel");
    for(const auto action:{app::MessageConfigurationAction::insert_echo,app::MessageConfigurationAction::delete_echo,app::MessageConfigurationAction::move_echo_up,app::MessageConfigurationAction::move_echo_down}){
        c.action=action;c.echo.id={action==app::MessageConfigurationAction::insert_echo?0:7};c.echo.revision=c.echo.id.value?3:0;
        c.insert_before={2};c.insert_before_revision=4;
        bytes=app::encode_message_configuration_change(c);const auto decoded=app::decode_message_configuration_change(bytes);
        require(!bytes.empty() && decoded && app::encode_message_configuration_change(*decoded)==bytes,"IPC27 echo administration canonical roundtrip");
        for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_configuration_change({bytes.begin(),bytes.begin()+n}),"IPC27 admin truncation rejected");
        auto extra=bytes;extra.push_back(1);require(!app::decode_message_configuration_change(extra),"IPC27 admin trailing authority byte rejected");
        if(action!=app::MessageConfigurationAction::insert_echo){c.echo.revision=0;require(app::encode_message_configuration_change(c).empty(),"IPC27 purge/move requires selected echo revision");}
    }
    c={};c.operation.bytes[0]=1;c.expected_configuration_revision=3;c.action=app::MessageConfigurationAction::delete_group;c.group.id={7};c.group.revision=2;
    bytes=app::encode_message_configuration_change(c);auto group_decoded=app::decode_message_configuration_change(bytes);
    require(!bytes.empty() && group_decoded && group_decoded->action==c.action && group_decoded->group.id==c.group.id && group_decoded->group.revision==2 && app::encode_message_configuration_change(*group_decoded)==bytes,"IPC28 empty-group deletion canonical roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_configuration_change({bytes.begin(),bytes.begin()+n}),"IPC28 group delete truncation rejected");
    auto extra=bytes;extra.push_back(1);require(!app::decode_message_configuration_change(extra),"IPC28 group delete trailing byte refused");
    c.group.revision=0;require(app::encode_message_configuration_change(c).empty(),"group delete requires selected revision");c.group.revision=2;c.group.id={0};require(app::encode_message_configuration_change(c).empty(),"unsaved group cannot be deleted");
    app::LocalMessageConfigurationSnapshot s;s.code=app::LocalAccountResultCode::ok;s.catalog.revision=1;s.catalog.assignment_revision=1;s.catalog.private_presentation_revision=1;
    s.catalog.groups={{{1},"Local",0,1,1,true}};s.catalog.line_limits={{10,200,1},{999,1000,1}};
    bytes=app::encode_message_configuration_snapshot(s);require(app::decode_message_configuration_snapshot(bytes)->catalog.line_limits.size()==2,"active finite message limits snapshot roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_configuration_snapshot({bytes.begin(),bytes.begin()+n}),"message snapshot truncation rejected");
    s.catalog.line_limits.push_back({10,200,1});require(app::encode_message_configuration_snapshot(s).empty(),"duplicate/unsorted limit identity rejected");
    app::LocalMessageConfigurationResult result{app::LocalAccountResultCode::ok,{2,1,2,true}};
    require(app::decode_message_configuration_result(app::encode_message_configuration_result(result))->result.replayed,"configuration replay result remains explicit");
    result.code=app::LocalAccountResultCode::conflict;require(app::encode_message_configuration_result(result).size()==1,"refusal serializes no stale success identity");
    c={};c.operation.bytes[0]=1;c.expected_configuration_revision=1;c.action=app::MessageConfigurationAction::private_presentation;c.expected_private_presentation_revision=1;c.private_pipe_colors=true;
    bytes=app::encode_message_configuration_change(c);const auto private_change=app::decode_message_configuration_change(bytes);
    require(private_change && private_change->private_pipe_colors && private_change->expected_private_presentation_revision==1 && app::encode_message_configuration_change(*private_change)==bytes,"IPC29 private presentation exact roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_configuration_change({bytes.begin(),bytes.begin()+n}),"IPC29 private policy truncation");
    extra=bytes;extra.push_back(0);require(!app::decode_message_configuration_change(extra),"IPC29 private policy trailing data");
    extra=bytes;extra.back()=2;require(!app::decode_message_configuration_change(extra),"IPC29 Boolean strict");
    c.presentation_mask=1;require(app::encode_message_configuration_change(c).empty(),"private policy cannot carry echo mask");
    c={};c.operation.bytes[0]=1;c.expected_configuration_revision=1;c.action=app::MessageConfigurationAction::echo;c.echo={{2},{1},2,"Public",1,1,{1,1,1},true,true};c.presentation_mask=1;
    bytes=app::encode_message_configuration_change(c);const auto echo_change=app::decode_message_configuration_change(bytes);
    require(echo_change && echo_change->presentation_mask==1 && echo_change->echo.renegade_pipe_colors,"IPC29 echo policy and explicit field mask");
    c.presentation_mask=2;require(app::encode_message_configuration_change(c).empty(),"unrecognized presentation mask rejected");
}
#include "message_maintenance_protocol_checks.inc"
}  // namespace

int main() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    try {
        std::cerr << "frame tests\n";
        frame_tests();
        message_configuration_protocol_tests();
        message_maintenance_protocol_checks();
        account_administration_protocol_tests();
        std::cerr << "hostname tests\n";
        l2_hostname_status_tests();
        std::cerr << "payload tests\n";
        payload_tests();
        std::cerr << "bootstrap tests\n";
        bootstrap_prerequisite_tests();
        std::cerr << "settings tests\n";
        c2_settings_protocol_tests();
        std::cerr << "scheduler tests\n";
        scheduler_protocol_tests();
        std::cerr << "stage4 tests\n";
        stage4_protocol_tests();
        std::cerr << "typed status tests\n";
        l1_typed_status_tests();
        reporting_protocol_tests();
        high_node_status_transfer_tests();
        {
            app::ManualClamAvNotice notice;notice.operation.bytes[0]=1;
            const auto started=app::encode_manual_clamav_notice(notice);
            require(!started.empty()&&app::decode_manual_clamav_notice(started).has_value(),"manual AV started notice round trip");
            for(unsigned phase=1;phase<=3;++phase){notice.phase=static_cast<app::ManualClamAvPhase>(phase);notice.attempt_utc=1792339200;
                notice.diagnostic=phase==1?"clamav_definitions_verified":phase==2?"clamav_update_vendor_cooldown":"clamav_update_cancelled";
                const auto encoded=app::encode_manual_clamav_notice(notice);const auto decoded=app::decode_manual_clamav_notice(encoded);
                require(decoded&&decoded->diagnostic==notice.diagnostic&&decoded->attempt_utc==notice.attempt_utc,"bounded manual AV outcome round trip");
                auto extra=encoded;extra.push_back(0);require(!app::decode_manual_clamav_notice(extra),"manual AV rejects trailing bytes");
                for(std::size_t n=0;n<encoded.size();++n)require(!app::decode_manual_clamav_notice({encoded.begin(),encoded.begin()+n}),"manual AV rejects every truncation");
            }
            for(const auto diagnostic:{"scan_admission_memory_headroom","scan_admission_commit_headroom","scan_admission_physical_headroom","file_receive_reservation_failed"}){
                notice.phase=app::ManualClamAvPhase::failed;notice.diagnostic=diagnostic;
                const auto encoded=app::encode_manual_clamav_notice(notice);const auto decoded=app::decode_manual_clamav_notice(encoded);
                require(decoded&&decoded->phase==notice.phase&&decoded->diagnostic==notice.diagnostic,"shared admission/file FAILURE survives final Event codec");
                notice.phase=app::ManualClamAvPhase::verified;require(app::encode_manual_clamav_notice(notice).empty(),"shared admission/file error can never forge verified success");
            }
            notice.phase=app::ManualClamAvPhase::failed;
            for(const auto diagnostic:{"other_memory_error","scan_bad\r\nspoof","file_bad\x1b[0m","scan_bad|spoof"}){notice.diagnostic=diagnostic;require(app::encode_manual_clamav_notice(notice).empty(),"manual AV rejects foreign namespace/terminal/log injection");}
            notice.diagnostic="clamav_bad\r\nspoof";require(app::encode_manual_clamav_notice(notice).empty(),"manual AV rejects terminal/log injection");
            notice.operation={};require(app::encode_manual_clamav_notice(notice).empty(),"manual AV refuses missing operation identity");
        }
        {
            app::FtnRuntimeRequest request;request.action=app::FtnRuntimeAction::poll;request.network="TEST";request.link="HUB";
            const auto wire=app::encode_ftn_runtime_request(request);require(app::decode_ftn_runtime_request(wire).has_value(),"explicit named FTN poll round trip");
            for(std::size_t n=0;n<wire.size();++n)require(!app::decode_ftn_runtime_request({wire.begin(),wire.begin()+n}),"FTN request rejects every truncation");
            request.network.clear();require(app::encode_ftn_runtime_request(request).empty(),"no unnamed FTN poll");request={};request.include_hold=true;require(app::encode_ftn_runtime_request(request).empty(),"hold inclusion never smuggled into status");
            app::FtnRuntimeResponse snapshot;snapshot.code=app::FtnConfigurationCode::ok;snapshot.mode=app::FtnMailerMode::native;snapshot.saved_revision=4;snapshot.active_revision=3;snapshot.capable=true;snapshot.serial_recovery_required=true;snapshot.diagnostic="ftn_message_id_history_needs_recovery";
            app::FtnSessionStatus session;session.serial=9;session.network="TEST";session.link="HUB";session.active=true;session.outbound=true;session.sent_bytes=2048;session.send={"mail.pkt",8192,2048,true,false};snapshot.sessions.push_back(session);snapshot.connections.push_back({"TEST","HUB","Named Hub",true,true});snapshot.queues.emplace();snapshot.queues->outbound[2]=1;snapshot.queues->unscanned=2;app::FtnQueueRow row;row.id.bytes[0]=1;row.outbound=true;row.network="TEST";row.link="HUB";row.file="mail.pkt";row.state=2;row.bytes=8192;snapshot.queues->rows.push_back(row);
            snapshot.activity.push_back({1793440000,9,"FidoNet","Synthetic Hub","Connecting to 127.0.0.1:24777."});
            snapshot.sessions.front().peer_address="2:100/2";snapshot.sessions.front().endpoint="127.0.0.1:26490";
            const auto bytes=app::encode_ftn_runtime_response(snapshot);const auto restored=app::decode_ftn_runtime_response(bytes);require(restored&&restored->serial_recovery_required&&restored->saved_revision==4&&restored->active_revision==3&&restored->sessions.front().peer_address=="2:100/2"&&restored->sessions.front().endpoint=="127.0.0.1:26490"&&restored->sessions.front().send.position==2048&&restored->queues->outbound[2]==1,"actual revision hold file progress and queue states retained separately");
            {
                auto recent=snapshot;
                auto& recent_session=recent.sessions.front();
                recent_session.last_sent={"acknowledged.pkt",39,39,false,false};
                recent_session.last_received={"received.pkt",75,75,false,false};
                const auto roundtrip=app::decode_ftn_runtime_response(app::encode_ftn_runtime_response(recent));
                require(roundtrip&&roundtrip->sessions.front().last_sent.name=="acknowledged.pkt"&&
                    roundtrip->sessions.front().last_sent.position==39&&roundtrip->sessions.front().last_received.size==75&&
                    roundtrip->sessions.front().send.position==2048,"completed files round-trip independently of live progress");
                recent_session.last_sent.position=1;
                require(app::encode_ftn_runtime_response(recent).empty(),"incomplete file cannot be encoded as a completion");
            }
            require(restored->activity.size()==1&&restored->activity[0].message==snapshot.activity[0].message&&restored->activity[0].utc_seconds==1793440000,"timestamped activity round trip");
            {auto bad=snapshot;bad.activity[0].message="forged\r\nline";require(app::encode_ftn_runtime_response(bad).empty(),"activity rejects control injection");bad=snapshot;bad.activity.resize(33,bad.activity[0]);require(app::encode_ftn_runtime_response(bad).empty(),"activity history bounded");}
            for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_ftn_runtime_response({bytes.begin(),bytes.begin()+n}),"FTN snapshot rejects every truncation");
            auto extra=bytes;extra.push_back(0);require(!app::decode_ftn_runtime_response(extra),"FTN snapshot rejects trailing bytes");snapshot.sessions.front().send.position=9000;require(app::encode_ftn_runtime_response(snapshot).empty(),"FTN file progress cannot exceed declared bytes");snapshot.sessions.front().send.position=2048;snapshot.diagnostic="ftn_ok\r\nforged";require(app::encode_ftn_runtime_response(snapshot).empty(),"FTN status rejects control injection");snapshot.diagnostic.clear();snapshot.sessions.resize(17);require(app::encode_ftn_runtime_response(snapshot).empty(),"FTN session count bounded");
        }
        #include "ftn_held_protocol_checks.inc"
        std::cout << "SFX Alpha local-control protocol tests PASS: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Alpha local-control protocol tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
