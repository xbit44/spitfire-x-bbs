#pragma once
#include "sfx/application/ftn_work.hpp"
namespace sfx::application {
inline std::string ftn_recovery_operation(const Identifier128& id){
    std::string text;for(const auto byte:id.bytes){text+="0123456789abcdef"[byte>>4];text+="0123456789abcdef"[byte&15];}return text;
}
inline std::string ftn_recovery_report_name(const FtnSerialRecoveryReceipt& r){return "FTN-Recovery-"+ftn_recovery_operation(r.operation)+".log";}
inline std::string ftn_recovery_report_text(const FtnSerialRecoveryReceipt& r){
    return "SFX FTN history recovery\r\nOperation: "+ftn_recovery_operation(r.operation)+
        "\r\nUTC seconds: "+std::to_string(r.utc)+"\r\nRecovery: "+std::to_string(r.generation)+
        "\r\nPrevious counter floor: "+std::to_string(r.old_floor)+"\r\nPreserved counter floor: "+std::to_string(r.new_floor)+
        "\r\nAdditional prior history supplied to this action: "+(r.prior_known?"verified":"not supplied")+
        "\r\nResult: FTN history recovery committed. Existing IDs, queue and completed states preserved.\r\n"
        "Separate held outbound items: "+std::to_string(r.held_outbound)+
        "\r\nSeparate held incoming items: "+std::to_string(r.held_ingress)+
        "\r\nSeparate queue holds remain in their recorded states. BBS remains stopped.\r\n";
}
}
