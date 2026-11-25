#include "sfx/application/durable_journal.hpp"
#include <algorithm>

namespace sfx::application {
namespace {
bool complete_boundary(IDurableJournalFiles& files, DurableJournalKind kind, const JournalFileState& state) {
    if (!state.bytes) return true;
    const std::string header = kind == DurableJournalKind::callers ? "; SFX CALLERS.LOG v1\r\n" : "; SFX HEYSYSOP.LOG v1\r\n";
    const std::string tail = kind == DurableJournalKind::callers ? "\r\n---\r\n" : "\r\n";
    if (state.bytes == header.size()) return files.read(kind, 0, header.size()) == header;
    return state.bytes >= tail.size() && files.read(kind, state.bytes - tail.size(), tail.size()) == tail;
}
}
bool DurableJournalPublisher::complete_intent(const CallerJournalPublication& intent) {
    auto state = files_.inspect(DurableJournalKind::callers);
    if (!state || state->identity != intent.file_identity || state->bytes < intent.byte_offset ||
        state->bytes > intent.byte_offset + intent.block.size()) return false;
    const auto already = static_cast<std::size_t>(state->bytes - intent.byte_offset);
    const auto prefix = files_.read(DurableJournalKind::callers, intent.byte_offset, already);
    if (!prefix || *prefix != intent.block.substr(0, already)) return false;
    // Timeout/failed append is not proof of no write. The durable intent and the
    // exact matching prefix decide how much (if any) remains after restart.
    if (!files_.append(DurableJournalKind::callers, *state,
            std::string_view(intent.block).substr(already))) return false;
    return storage_.acknowledge_caller_publication(intent.caller_number).succeeded();
}
JournalPublishResult DurableJournalPublisher::publish_one_caller() noexcept {
    try {
        const auto pending = storage_.caller_publication();
        if (!pending || !pending.value) return JournalPublishResult::unavailable;
        if (*pending.value) return complete_intent(**pending.value)
            ? JournalPublishResult::published : JournalPublishResult::unavailable;
        auto page = storage_.pending_caller_journals(false, 1);
        if (!page || !page.value) return JournalPublishResult::unavailable;
        if (page.value->empty()) return JournalPublishResult::no_record;
        auto record = std::move(page.value->front());
        if (record.recovered && record.session.completed_utc_seconds) {
            // An interrupted process cannot prove the wall-clock time of its
            // final transition beyond the durable accounting checkpoint.
            const auto end = *record.session.completed_utc_seconds;
            record.activities.erase(std::remove_if(record.activities.begin(), record.activities.end(),
                [end](const auto& activity) { return activity.utc_seconds > end; }), record.activities.end());
        }
        const auto block = render_caller_journal(record, time_);
        const auto maximum=static_cast<std::uint64_t>(limits_.callers.size_mib)*1024*1024;
        auto state = files_.inspect(DurableJournalKind::callers);
        if (!block || !state || state->bytes > kDurableJournalMaximumFileBytes ||
            !complete_boundary(files_, DurableJournalKind::callers, *state)) return JournalPublishResult::unavailable;
        if (state->bytes>maximum || block->size() > maximum - state->bytes) {
            if (!files_.archive_and_reopen(DurableJournalKind::callers)) return JournalPublishResult::unavailable;
            state = files_.inspect(DurableJournalKind::callers);
            if (!state || state->bytes > maximum ||
                block->size() > maximum - state->bytes) return JournalPublishResult::unavailable;
        }
        CallerJournalPublication intent{record.caller_number, state->identity, state->bytes, *block};
        if (!storage_.prepare_caller_publication(intent)) return JournalPublishResult::unavailable;
        return complete_intent(intent) ? JournalPublishResult::published : JournalPublishResult::unavailable;
    } catch (...) { return JournalPublishResult::unavailable; }
}
bool DurableJournalPublisher::publish_attention(const AttentionRecord& record) noexcept {
    try {
        const auto block = render_attention_journal(record, time_);
        const auto maximum=static_cast<std::uint64_t>(limits_.attention.size_mib)*1024*1024;
        auto state = files_.inspect(DurableJournalKind::attention);
        if (!block || !state || state->bytes > kDurableJournalMaximumFileBytes ||
            !complete_boundary(files_, DurableJournalKind::attention, *state)) return false;
        if (state->bytes>maximum || block->size() > maximum - state->bytes) {
            if (!files_.archive_and_reopen(DurableJournalKind::attention)) return false;
            state = files_.inspect(DurableJournalKind::attention);
        }
        return state && state->bytes <= maximum &&
            block->size() <= maximum - state->bytes &&
            files_.append(DurableJournalKind::attention, *state, *block);
    } catch (...) { return false; }
}
bool DurableJournalPublisher::archive_and_reopen(const DurableJournalKind kind, const bool confirmed) noexcept {
    if (!confirmed) return false;
    try {
        if (kind == DurableJournalKind::callers) {
            const auto pending = storage_.caller_publication();
            if (!pending || !pending.value || *pending.value) return false;
        }
        return files_.archive_and_reopen(kind);
    } catch (...) { return false; }
}
bool DurableJournalPublisher::publish_other_board(const OtherBoardLogRecord& record) noexcept {
    try {
        if(record.action==ClassicInformationAction::opening_date || !record.event_id.valid() || record.caller<0 ||
            record.board_name.empty() || record.board_name.size()>kOtherBoardNameMaximum || record.actor_name.empty() ||
            record.actor_name.size()>30 || !valid_utc_seconds(record.utc_seconds))return false;
        for(const auto* field:{&record.board_name,&record.actor_name})
            for(unsigned char byte:*field)if(byte<32 || byte>126)return false;
        const auto timestamp=time_?time_(record.utc_seconds):std::nullopt;
        if(!timestamp || timestamp->size()!=19)return false;
        const auto action=record.action==ClassicInformationAction::add_board?"ADDED":
            record.action==ClassicInformationAction::remove_board?"REMOVED":"EDITED";
        std::string event;constexpr char hex[]="0123456789ABCDEF";
        for(auto byte:record.event_id.bytes){event+=hex[byte>>4];event+=hex[byte&15];}
        const std::string text="OTHER_BBS_"+std::string(action)+" at "+*timestamp+"\r\nActor: "+record.actor_name+
            (record.caller?" (account "+std::to_string(record.caller)+")":" (installation owner)")+
            "\r\nBBS: "+record.board_name+"\r\nEVENT "+event+"\r\n";
        // Do not insert a standalone action inside an interrupted CALLERS block.
        // Finish its existing durable publication first, using the same writer.
        const auto pending=storage_.caller_publication();
        if(!pending || !pending.value || (*pending.value && !complete_intent(**pending.value)))return false;
        const auto append=[&](DurableJournalKind kind,std::string_view block,std::uint64_t maximum) {
            auto state=files_.inspect(kind);
            if(!state || !complete_boundary(files_,kind,*state))return false;
            if(state->bytes>maximum || block.size()>maximum-state->bytes) {
                if(!files_.archive_and_reopen(kind))return false;
                state=files_.inspect(kind);
            }
            return state && state->bytes<=maximum && block.size()<=maximum-state->bytes && files_.append(kind,*state,block);
        };
        const bool caller=append(DurableJournalKind::callers,text+"\r\n---\r\n",
            static_cast<std::uint64_t>(limits_.callers.size_mib)*1024*1024);
        const bool attention=record.action!=ClassicInformationAction::remove_board ||
            append(DurableJournalKind::attention,text,static_cast<std::uint64_t>(limits_.attention.size_mib)*1024*1024);
        return caller && attention;
    }catch(...){return false;}
}
} // namespace sfx::application
