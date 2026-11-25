#pragma once

#include "sfx/application/caller_journal.hpp"
#include "sfx/application/botgate.hpp"
#include "sfx/application/classic_information_storage.hpp"

namespace sfx::application {
enum class DurableJournalKind { callers, attention, runtime };
struct JournalFileState { std::string identity; std::uint64_t bytes{0}; };

// Owned by one Runtime publication worker. Platform implementations only accept
// these two canonical leaves; no caller-controlled path or unbounded file read.
class IDurableJournalFiles {
public:
    virtual ~IDurableJournalFiles() = default;
    virtual std::optional<JournalFileState> inspect(DurableJournalKind) = 0;
    virtual std::optional<std::string> read(DurableJournalKind, std::uint64_t offset, std::size_t bytes) = 0;
    virtual bool append(DurableJournalKind, const JournalFileState& expected, std::string_view suffix) = 0;
    // Must preserve the active file as an archive, create/reopen a fresh active
    // file, then retain at most ten recognized owned archives. Failure is visible
    // and never authorizes truncation or deleting an active/unrelated file.
    virtual bool archive_and_reopen(DurableJournalKind) = 0;
};
enum class JournalPublishResult { published, no_record, unavailable };

// Deliberately single-threaded: the host supplies exactly one serialized worker
// for both files. Database operations use the existing authoritative writer.
class DurableJournalPublisher final {
public:
    DurableJournalPublisher(ICallerJournalStorage& storage, IDurableJournalFiles& files, JournalLocalTime time)
        : storage_(storage), files_(files), time_(std::move(time)) {}
    JournalPublishResult publish_one_caller() noexcept;
    bool publish_attention(const AttentionRecord&) noexcept;
    bool publish_other_board(const OtherBoardLogRecord&) noexcept;
    bool configure(const LoggingConfiguration& value) noexcept {
        if (!valid_logging_configuration(value)) return false;
        limits_=value; return true;
    }
    // Future confirmed Sysop operation, not a UI command in this pass.
    bool archive_and_reopen(DurableJournalKind, bool explicitly_confirmed) noexcept;
private:
    bool complete_intent(const CallerJournalPublication&);
    ICallerJournalStorage& storage_;
    IDurableJournalFiles& files_;
    JournalLocalTime time_;
    LoggingConfiguration limits_;
};
} // namespace sfx::application
