#include "sfx/application/operational_backlog.hpp"
#include <algorithm>
#include <limits>

namespace sfx::application {
namespace {
bool valid_record(const OperationalEncodedEntry& item, Identifier128 epoch) {
    if (!item.sequence || item.record.size() > kOperationalRecordMaximum) return false;
    const auto decoded = decode_operational_record(item.record, item.sequence);
    return decoded && decoded->event && decoded->event->context.epoch == epoch;
}
}

bool valid_operational_page(const OperationalLogPage& page) noexcept {
    try {
        if (!page.cursor.epoch.valid() || page.cursor.after > page.newest ||
            page.entries.size() > kOperationalPageEntries || page.oldest > page.newest ||
            (!page.oldest != !page.newest) ||
            (page.oldest && page.newest - page.oldest >= kOperationalBacklogEntries) ||
            page.gap != (page.oldest && page.cursor.after < page.oldest - 1)) return false;
        std::size_t bytes = kOperationalPageHeaderBytes;
        auto sequence = page.gap ? page.oldest - 1 : page.cursor.after;
        std::uint64_t origin = 0;
        for (const auto& entry : page.entries) {
            if (sequence == UINT64_MAX || entry.sequence != sequence + 1 ||
                entry.sequence > page.newest || !valid_record(entry, page.cursor.epoch)) return false;
            const auto record = decode_operational_record(entry.record, entry.sequence);
            if (record->event->context.origin_sequence <= origin) return false;
            origin = record->event->context.origin_sequence;
            sequence = entry.sequence;
            bytes += 10 + entry.record.size();
        }
        return bytes <= kOperationalPageBytes && page.next == sequence &&
            (!page.entries.empty() || page.cursor.after == page.newest);
    } catch (...) { return false; }
}

bool OperationalBacklog::append(OperationalEncodedEntry entry) {
    if (!epoch_.valid() || !valid_record(entry, epoch_) ||
        (!entries_.empty() && (newest() == UINT64_MAX || entry.sequence != newest() + 1))) return false;
    const auto decoded = decode_operational_record(entry.record, entry.sequence);
    const auto origin = decoded->event->context.origin_sequence;
    if (origin <= last_origin_) return false;
    const auto size = entry.record.size();
    entries_.push_back(std::move(entry)); // allocate before mutating accounting
    last_origin_ = origin;
    bytes_ += size;
    while (entries_.size() > kOperationalBacklogEntries || bytes_ > kOperationalBacklogBytes) {
        bytes_ -= entries_.front().record.size();
        entries_.pop_front();
    }
    return true;
}

std::optional<OperationalLogPage> OperationalBacklog::page(OperationalLogCursor cursor) const {
    if (!epoch_.valid() || cursor.epoch != epoch_ || cursor.after > newest()) return std::nullopt;
    OperationalLogPage result{cursor, oldest(), newest(), cursor.after, false, {}};
    result.gap = result.oldest && cursor.after < result.oldest - 1;
    std::size_t bytes = kOperationalPageHeaderBytes;
    // Sequences are contiguous, so seek directly without scanning 10,000 rows
    // for every page or copying the full history under the producer's lock.
    const auto offset = cursor.after < result.oldest ? 0 :
        static_cast<std::size_t>(cursor.after - result.oldest + 1);
    for (auto i = offset; i < entries_.size(); ++i) {
        const auto& item = entries_[i];
        if (result.entries.size() == kOperationalPageEntries ||
            bytes + 10 + item.record.size() > kOperationalPageBytes) break;
        result.entries.push_back(item);
        bytes += 10 + item.record.size();
        result.next = item.sequence;
    }
    return result;
}

std::vector<AlphaOperationalLogEntry> OperationalBacklog::tail(std::size_t count) const {
    std::vector<AlphaOperationalLogEntry> result;
    for (auto i = entries_.size() - std::min(count, entries_.size()); i < entries_.size(); ++i)
        result.push_back(*decode_operational_record(entries_[i].record, entries_[i].sequence));
    return result;
}

void OperationalLogCache::bind(Identifier128 epoch) {
    epoch_ = epoch;
    high_water_ = 0;
    clear_presentation();
}
void OperationalLogCache::clear_presentation() {
    backlog_ = OperationalBacklog(epoch_);
    cursor_ = 0; gap_ = false;
}
bool OperationalLogCache::accept(const OperationalLogPage& page) {
    if (page.cursor.epoch != epoch_ || page.cursor.after != cursor_ ||
        page.newest < high_water_ || !valid_operational_page(page)) return false;
    if (!page.entries.empty() && cursor_ && decode_operational_record(
            page.entries.front().record, page.entries.front().sequence)->event->context.origin_sequence <=
            backlog_.last_origin()) return false;
    if (page.gap) { backlog_ = OperationalBacklog(epoch_); gap_ = true; }
    for (const auto& item : page.entries) {
        if (!backlog_.append(item)) return false;
    }
    cursor_ = page.next;
    high_water_ = page.newest;
    return true;
}
} // namespace sfx::application
