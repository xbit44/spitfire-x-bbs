#pragma once
#include "sfx/application/account_storage.hpp"

namespace sfx::application {
inline constexpr std::size_t kOtherBoardNameMaximum=64, kOtherBoardAddressMaximum=255;
inline constexpr std::size_t kOtherBoardPageSize=32, kOtherBoardMaximum=999;
struct OtherBoardValues { std::string name,telnet,ssh; };
struct OtherBoardRecord { std::int64_t id{},revision{}; OtherBoardValues values; };
struct OtherBoardPage { std::vector<OtherBoardRecord> entries; bool has_more{}; };
enum class ClassicInformationAction : std::uint8_t { add_board, edit_board, remove_board, opening_date };
struct ClassicInformationChange {
    ClassicInformationAction action{ClassicInformationAction::add_board};
    std::int64_t id{},expected_revision{};
    OtherBoardValues values;
    std::string date;
};
// Authority is established in the host and is NEVER part of the IPC payload.
struct ClassicInformationActor {
    bool installation_owner{};
    AccountId caller{};
    std::int64_t caller_revision{},now_utc{};
    Identifier128 event_id;
};
struct OtherBoardLogRecord {
    ClassicInformationAction action{ClassicInformationAction::add_board};
    std::string board_name,actor_name;
    AccountId caller{};
    std::int64_t utc_seconds{};
    Identifier128 event_id;
};
// The catalog is owner-editable text. The host supplies the only filesystem
// implementation; account authority is checked by the existing database writer.
class IOtherBoardTextStorage {
public:
    virtual ~IOtherBoardTextStorage()=default;
    virtual StorageResult<OtherBoardPage> read(std::int64_t after)=0;
    virtual StorageResult<OtherBoardLogRecord> change(const ClassicInformationChange&,
        const ClassicInformationActor&,std::string_view actor_name)=0;
    // Called after the authority transaction ends; never wait on journal/database
    // publication while holding its writer transaction.
    virtual bool publish_log(const OtherBoardLogRecord&) noexcept=0;
};
inline constexpr std::size_t kOtherBoardTextMaximum=1024U*1024U;
std::optional<std::vector<OtherBoardValues>> parse_other_board_text(std::string_view);
std::string encode_other_board_text(const std::vector<OtherBoardValues>&);
struct OpeningDateRecord {
    std::int64_t revision{},initialization_utc{};
    std::optional<std::string> configured_date;
};
struct SystemInformationCounters { std::int64_t total_calls{},today_calls{},active_accounts{}; };
bool valid_other_board_values(const OtherBoardValues&) noexcept;
bool valid_classic_information_change(const ClassicInformationChange&) noexcept;
class IClassicInformationStorage {
public:
    virtual ~IClassicInformationStorage()=default;
    virtual StorageResult<OtherBoardPage> other_boards(std::int64_t after_id)=0;
    virtual StorageResult<OpeningDateRecord> opening_date()=0;
    virtual StorageStatus change_classic_information(const ClassicInformationChange&,const ClassicInformationActor&)=0;
    virtual StorageResult<SystemInformationCounters> system_information_counters(std::int64_t day_begin,std::int64_t day_end)=0;
};
} // namespace sfx::application
