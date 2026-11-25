#pragma once
#include "sfx/archive/qwk_packet_builder.hpp"
#include "sfx/application/qwk_reply_storage.hpp"
#include <string>
namespace sfx::apps::runtime {
inline std::string qwk_notice_lines(std::string_view text){
    // Only authored QWK feedback enters here, never packet content/diagnostics.
    std::string result;std::size_t column{};
    for(std::size_t at=0;at<text.size();){
        if(text[at]=='\n'){result+='\n';column=0;++at;continue;}
        if(text[at]==' '){++at;continue;}
        auto end=text.find_first_of(" \n",at);if(end==text.npos)end=text.size();const auto word=text.substr(at,end-at);
        if(column&&column+1+word.size()>76){result+='\n';column=0;}
        if(column){result+=' ';++column;}result.append(word);column+=word.size();at=end;
    }
    return result;
}
inline std::string qwk_preparation_notice_text(const archive::QwkPreparedDownload& packet){
    const std::string unchanged="\nNo read/pointer changes made.";
    if(packet.codec_error==application::QwkCodecError::unsupported_content)
        return "A selected character cannot be exported losslessly in classic CP437 QWK. If your reader supports it, choose UTF-8 with Full Headers in Packet Settings and retry."+unchanged;
    if(packet.result.error==archive::Error::limit_exceeded||packet.codec_error==application::QwkCodecError::limit_exceeded)
        return "A complete selected message cannot fit the packet limits. No message was truncated or skipped; contact the Sysop."+unchanged;
    if(packet.result.error==archive::Error::cancelled)
        return "Packet preparation cancelled or its time/authority limit changed. Please retry when available."+unchanged;
    if(packet.storage.code==application::StorageErrorCode::busy||packet.storage.code==application::StorageErrorCode::backpressure)
        return "BBS storage is busy. Please retry packet preparation shortly."+unchanged;
    if(packet.storage.code==application::StorageErrorCode::io||packet.storage.code==application::StorageErrorCode::corrupt||packet.storage.code==application::StorageErrorCode::full||packet.storage.code==application::StorageErrorCode::incompatible_schema||packet.storage.code==application::StorageErrorCode::migration_failed||packet.storage.code==application::StorageErrorCode::invalid_location||packet.storage.code==application::StorageErrorCode::internal||packet.storage.code==application::StorageErrorCode::shutting_down)
        return "BBS storage is unavailable. Please retry or contact the Sysop."+unchanged;
    if(packet.storage.code==application::StorageErrorCode::conflict||packet.storage.code==application::StorageErrorCode::permission_denied||packet.storage.code==application::StorageErrorCode::not_found||packet.result.error==archive::Error::source_changed)
        return "Packet messages, settings or access changed during preparation. Please retry."+unchanged;
    if(packet.codec_error==application::QwkCodecError::invalid_record||packet.codec_error==application::QwkCodecError::invalid_metadata)
        return "A selected message has unsupported QWK metadata. No messages were skipped; contact the Sysop."+unchanged;
    return "Packet preparation is unavailable. Please retry or contact the Sysop."+unchanged;
}
inline std::string qwk_import_notice_text(const application::StorageStatus& failure,bool absence_confirmed){
    if(!absence_confirmed)return "Reply packet was not accepted or its outcome could not be confirmed. No import success is claimed.";
    const std::string suffix="\nNothing imported.";
    if(failure.operation=="FTN network is ambiguous; use online NetMail to select the network")
        return "More than one FTN network can route this NetMail. Use online NetMail to select the network, or reply to downloaded NetMail with its original reply link."+suffix;
    if(failure.operation=="No configured FTN route for reply destination")
        return "No configured FTN network can route this NetMail destination. Check the address with the Sysop."+suffix;
    if(failure.code==application::StorageErrorCode::invalid_argument)
        return "Reply validation refused an author, recipient, conference, addressing field or message. Check your reader's QWK/QWKE settings and packet contents."+suffix;
    if(failure.code==application::StorageErrorCode::conflict||failure.code==application::StorageErrorCode::permission_denied||failure.code==application::StorageErrorCode::not_found)
        return "Reply access, settings or message targets changed. Review the packet and retry with current access."+suffix;
    return "Reply import could not complete. Please retry or contact the Sysop."+suffix;
}
inline std::string qwk_preparation_notice(const archive::QwkPreparedDownload& packet){return qwk_notice_lines(qwk_preparation_notice_text(packet));}
inline std::string qwk_import_notice(const application::StorageStatus& failure,bool absence_confirmed){return qwk_notice_lines(qwk_import_notice_text(failure,absence_confirmed));}
}
