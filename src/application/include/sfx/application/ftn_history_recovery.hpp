#pragma once
#include "sfx/application/ftn_work.hpp"
#include <memory>
namespace sfx::application {
inline std::string ftn_history_recovery_problem(std::string_view reason){
    if(reason.find("stopped")!=reason.npos||reason.find("exclusive")!=reason.npos)
        return "Stop the BBS and finish other maintenance before trying again.";
    if(reason.find("external")!=reason.npos||reason.find("busy_file")!=reason.npos)
        return "Stop independently launched BinkD and review its busy-file status before trying again. SFX kept the busy file.";
    if(reason.find("outbound")!=reason.npos||reason.find("ingress")!=reason.npos||reason.find("queue")!=reason.npos)
        return "A required queued file could not be verified. Keep the board stopped, preserve the files, and follow BBS Health Check's matching recovery steps.";
    if(reason.find("clock")!=reason.npos||reason.find("serial_range")!=reason.npos)
        return "Check the computer's date and time and the FTN message ID range. Nothing was reset.";
    if(reason.find("installation")!=reason.npos||reason.find("portable")!=reason.npos)
        return "Installation ownership or file records could not be verified. Use the supported copy/restore preparation and BBS Health Check.";
    if(reason.find("database")!=reason.npos||reason.find("history")!=reason.npos||reason.find("schema")!=reason.npos)
        return "Saved message ID history could not be verified. Preserve the board and review BBS Health Check or a matching verified backup.";
    return "FTN recovery could not finish. Keep the BBS stopped, preserve the board, and review BBS Health Check and Logs.";
}
struct FtnHistoryRecoveryPreview {
    FtnSerialState state;
    std::int64_t utc{};
    std::size_t pending_outbound{},pending_ingress{};
    std::uint64_t held_outbound{},held_ingress{};
};
class IFtnHistoryRecovery {
public:
    virtual ~IFtnHistoryRecovery()=default;
    virtual const FtnHistoryRecoveryPreview& preview()const noexcept=0;
    virtual const std::string& confirmation_token()const noexcept=0;
    virtual FtnHistoryRecoveryResult execute(std::string_view confirmation)=0;
};
}
