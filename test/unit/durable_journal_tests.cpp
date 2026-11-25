#include "sfx/application/durable_journal.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
namespace app = sfx::application;
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
app::CallerJournalRecord record() {
    app::CallerJournalRecord r; r.caller_number = 7; r.display_name = "Safe Caller";
    r.transport = "SSH"; r.source_ip = "127.0.0.1"; r.ansi = 1;
    r.session.session_id.bytes[0] = 1; r.session.node_id = 4;
    r.session.started_utc_seconds = 1788990000; r.session.completed_utc_seconds = 1788990008;
    r.session.persisted_seconds = 8; r.session.outcome = app::SessionOutcomeCode::normal;
    r.activities = {{app::CallerActivityKind::successful_logon, 1788990000, {}},
        {app::CallerActivityKind::main_menu, 1788990001, {}}};
    return r;
}
app::JournalLocalTime time() { return [](std::int64_t) { return std::optional<std::string>("2026-09-09 13:00:00"); }; }
struct Storage : app::ICallerJournalStorage {
    app::CallerJournalRecord caller{record()};
    std::optional<app::CallerJournalPublication> intent;
    bool published{}, fail_ack{};
    unsigned prepares{}, acks{};
    app::StorageResult<std::vector<app::CallerJournalRecord>> pending_caller_journals(bool unfinished, std::size_t) override {
        return {app::StorageStatus::success(), published || unfinished ? std::vector<app::CallerJournalRecord>{} : std::vector<app::CallerJournalRecord>{caller}};
    }
    app::StorageStatus append_caller_activity(app::Identifier128, const app::CallerActivity&, std::string_view) override { return {}; }
    app::StorageStatus mark_caller_recovered(app::Identifier128) override { return {}; }
    app::StorageResult<std::optional<app::CallerJournalPublication>> caller_publication() override { return {app::StorageStatus::success(), intent}; }
    app::StorageStatus prepare_caller_publication(const app::CallerJournalPublication& p) override { ++prepares; intent = p; return {}; }
    app::StorageStatus acknowledge_caller_publication(std::int64_t n) override {
        ++acks;
        if (fail_ack || !intent || intent->caller_number != n) return {app::StorageErrorCode::io, "fixture"};
        published = true; intent.reset(); return {};
    }
};
struct Files : app::IDurableJournalFiles {
    std::array<std::string, 2> content{{"; SFX CALLERS.LOG v1\r\n", "; SFX HEYSYSOP.LOG v1\r\n"}};
    std::array<std::string, 2> identities{{"1:2", "1:3"}};
    std::array<std::vector<std::string>, 2> archives;
    bool fail_rotation{}, fail_append{}, fail_inspect{};
    std::size_t cut{};
    unsigned appends{}, rotations{}, empty_flushes{};
    static unsigned i(app::DurableJournalKind k) { return k == app::DurableJournalKind::callers ? 0U : 1U; }
    std::optional<app::JournalFileState> inspect(app::DurableJournalKind k) override {
        if (fail_inspect) return {};
        return app::JournalFileState{identities[i(k)], content[i(k)].size()};
    }
    std::optional<std::string> read(app::DurableJournalKind k, std::uint64_t offset, std::size_t bytes) override {
        if (offset > content[i(k)].size() || bytes > content[i(k)].size() - offset) return {};
        return content[i(k)].substr(static_cast<std::size_t>(offset), bytes);
    }
    bool append(app::DurableJournalKind k, const app::JournalFileState& expected, std::string_view text) override {
        ++appends; if (text.empty()) ++empty_flushes;
        if (expected.identity != identities[i(k)] || expected.bytes != content[i(k)].size()) return false;
        if (fail_append) { content[i(k)].append(text.substr(0, cut)); return false; }
        content[i(k)].append(text); return true;
    }
    bool archive_and_reopen(app::DurableJournalKind k) override {
        ++rotations; if (fail_rotation) return false;
        auto& old = archives[i(k)]; old.push_back(content[i(k)]);
        if (old.size() > app::kDurableJournalArchiveCount) old.erase(old.begin());
        content[i(k)] = i(k) ? "; SFX HEYSYSOP.LOG v1\r\n" : "; SFX CALLERS.LOG v1\r\n";
        identities[i(k)] = "1:" + std::to_string(10 + rotations); return true;
    }
};
void recovery() {
    const auto block = *app::render_caller_journal(record(), time());
    for (const auto cut : {std::size_t{0}, std::size_t{1}, block.size() / 2, block.size()}) {
        Storage s; Files f; f.fail_append = true; f.cut = cut;
        app::DurableJournalPublisher first(s, f, time());
        check(first.publish_one_caller() == app::JournalPublishResult::unavailable, "uncertain append not acknowledged");
        check(s.intent && !s.published && s.acks == 0, "intent persists across partial/complete failed append");
        f.fail_append = false;
        app::DurableJournalPublisher restarted(s, f, time());
        check(restarted.publish_one_caller() == app::JournalPublishResult::published, "restart appends matching remainder only");
        check(f.content[0] == "; SFX CALLERS.LOG v1\r\n" + block, "one exact noninterleaved block after recovery");
        check(restarted.publish_one_caller() == app::JournalPublishResult::no_record && s.acks == 1,
            "repeated recovery never emits second block");
        check(s.prepares == 1, "one durable append intent");
    }
    Storage s; Files f; s.fail_ack = true;
    app::DurableJournalPublisher publisher(s, f, time());
    check(publisher.publish_one_caller() == app::JournalPublishResult::unavailable, "acknowledgement failure preserved");
    const auto original = f.content[0]; s.fail_ack = false;
    check(publisher.publish_one_caller() == app::JournalPublishResult::published && f.content[0] == original &&
        f.empty_flushes == 1, "full-write/ack crash flushes and acknowledges without duplicate");
    for (unsigned damage = 0; damage < 3; ++damage) {
        Storage corrupt; Files file; file.fail_append = true; file.cut = 2;
        app::DurableJournalPublisher p(corrupt, file, time()); (void)p.publish_one_caller();
        file.fail_append = false;
        if (!damage) file.identities[0] = "1:999";
        if (damage == 1) file.content[0].back() = '?';
        if (damage == 2) file.content[0] += std::string(block.size(), 'z');
        const auto preserved = file.content[0];
        check(p.publish_one_caller() == app::JournalPublishResult::unavailable && corrupt.intent &&
            file.content[0] == preserved && file.appends == 1, "identity/prefix/size mismatch preserved, never blindly replayed");
    }
}
void bounds() {
    Storage s; Files f; app::DurableJournalPublisher p(s, f, time());
    const auto block = *app::render_caller_journal(record(), time());
    f.content[0] = std::string(static_cast<std::size_t>(app::kDurableJournalFileBytes - block.size() - 7), 'a') + "\r\n---\r\n";
    check(p.publish_one_caller() == app::JournalPublishResult::published && f.rotations == 0 &&
        f.content[0].size() == app::kDurableJournalFileBytes, "exact ten MiB complete boundary accepted");
    s.published = false; s.caller.caller_number = 8;
    const auto full = f.content[0]; f.fail_rotation = true;
    check(p.publish_one_caller() == app::JournalPublishResult::unavailable && f.content[0] == full && !s.intent,
        "rotation failure preserves full active file and authoritative pending caller");
    f.fail_rotation = false;
    check(p.publish_one_caller() == app::JournalPublishResult::published && f.archives[0].front() == full,
        "rotation precedes whole new block, preserving prior exact file");
    check(!p.archive_and_reopen(app::DurableJournalKind::callers, false), "future clear requires explicit confirmation");
    for (unsigned n = 0; n < 12; ++n) check(p.archive_and_reopen(app::DurableJournalKind::callers, true), "confirmed archive/reopen model");
    check(f.archives[0].size() == 10, "archive retention model bounded to newest ten");
    for (const auto k : {app::DurableJournalKind::callers, app::DurableJournalKind::attention}) {
        Storage partial; Files file; file.content[Files::i(k)] += "partial";
        app::DurableJournalPublisher writer(partial, file, time());
        app::AttentionRecord notice; notice.utc_seconds = 1788990000;
        check(k == app::DurableJournalKind::callers ? writer.publish_one_caller() == app::JournalPublishResult::unavailable :
            !writer.publish_attention(notice), "unknown incomplete active tail never appended or truncated");
        check(file.appends == 0 && file.rotations == 0 && !partial.intent, "incomplete tail preserves active file");
    }
    app::AttentionRecord event; event.utc_seconds = 1788990000;
    check(p.publish_attention(event), "serialized attention shares same writer");
    event.account_name = "unclassified";
    check(!p.publish_attention(event), "unclassified identity never published");
    Storage actions;Files action_files;app::DurableJournalPublisher action_writer(actions,action_files,time());
    app::OtherBoardLogRecord board;board.board_name="Test BBS";board.actor_name="Safe Caller";board.caller=4;
    board.utc_seconds=1788990000;board.event_id.bytes[0]=1;
    check(action_writer.publish_other_board(board),"Other BBS add publishes standalone caller log");
    check(action_files.content[0].find("OTHER_BBS_ADDED")!=std::string::npos &&
        action_files.content[0].find("Safe Caller (account 4)")!=std::string::npos &&
        action_files.content[1].find("OTHER_BBS_ADDED")==std::string::npos,"add actual identity/name only in CALLERS");
    board.action=app::ClassicInformationAction::remove_board;
    check(action_writer.publish_other_board(board),"Other BBS removal publishes both required logs");
    check(action_files.content[0].find("OTHER_BBS_REMOVED")!=std::string::npos &&
        action_files.content[1].find("OTHER_BBS_REMOVED")!=std::string::npos,"removal in CALLERS and HEYSYSOP");
    board.actor_name="forged\r\nactor";check(!action_writer.publish_other_board(board),"action log cannot inject lines");
}
}
int main() {
    try { recovery(); bounds(); std::cout << "PASS durable journal: " << checks << " checks; 0 failed; 0 skipped\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL after " << checks << ": " << e.what() << '\n'; return 1; }
}
