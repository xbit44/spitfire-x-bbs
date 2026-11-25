#include "sfx/application/file.hpp"
#include "sfx/application/qwk.hpp" // existing lossless CP437 codec; no QWK behavior changes
#include <algorithm>
#include <limits>

namespace sfx::application {
namespace {
#include "file_unicode.inc"
bool level(std::uint16_t n) noexcept { return n >= 1 && n <= 999; }
bool same(std::string_view a, std::string_view b) noexcept {
    std::size_t i{},j{};std::uint32_t x{},y{};
    while(i<a.size()&&j<b.size())if(!next_file_scalar(a,i,x)||!next_file_scalar(b,j,y)||fold_file_scalar(x)!=fold_file_scalar(y))return false;
    return i==a.size()&&j==b.size();
}
bool text_valid(std::string_view s, std::size_t maximum, bool multiline) noexcept {
    if (s.size() > maximum) return false;
    std::size_t at{};std::uint32_t c{};
    while(at<s.size())if(!next_file_scalar(s,at,c)||unsafe_file_scalar(c)||(c<32&&!(multiline&&c=='\n')))return false;
    return true;
}
bool name_text(std::string_view s) noexcept {
    return !s.empty() && s.front() != ' ' && s.back() != ' ' && text_valid(s, 64, false);
}
bool base_access(const FileGroup& g, const FileArea& a, FileAuthority caller) noexcept {
    return caller.valid() && g.enabled && a.enabled && g.id == a.group &&
        caller.level >= g.access && caller.level >= a.permissions.access;
}
std::uint64_t add_saturated(std::uint64_t a, std::uint64_t b) noexcept {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool contains(std::string_view haystack, std::string_view needle) noexcept {
    try{const auto h=file_case_key(haystack),n=file_case_key(needle);return h&&n&&h->find(*n)!=std::string::npos;}catch(...){return false;}
}
int compare_names(std::string_view a, std::string_view b) noexcept {
    std::size_t i{},j{};std::uint32_t x{},y{};
    while(i<a.size()&&j<b.size()){
        if(!next_file_scalar(a,i,x)||!next_file_scalar(b,j,y))return a.compare(b);
        x=fold_file_scalar(x);y=fold_file_scalar(y);
        if (x != y) return x < y ? -1 : 1;
    }
    return i==a.size()?(j==b.size()?0:-1):1;
}
} // namespace

bool valid_file_utf8(std::string_view text) noexcept {
    std::size_t at{};std::uint32_t scalar{};while(at<text.size())if(!next_file_scalar(text,at,scalar))return false;return true;
}
std::optional<std::string> file_case_key(std::string_view text){
    if(text.size()>kFileLongDescriptionBytes)return {};
    std::string result;result.reserve(text.size());std::size_t at{};std::uint32_t scalar{};
    while(at<text.size()){if(!next_file_scalar(text,at,scalar))return {};append_file_scalar(result,fold_file_scalar(scalar));}return result;
}

bool FileAuthority::valid() const noexcept { return level >= 1 && level <= 999 && sysop_threshold >= 1 && sysop_threshold <= 999; }
bool FileAuthority::sysop() const noexcept { return valid() && level >= sysop_threshold; }
bool valid_file_group(const FileGroup& g) noexcept { return g.id.value >= 0 && g.revision >= 1 && name_text(g.name) && level(g.access); }
bool valid_file_area(const FileArea& a) noexcept {
    return a.id.value >= 0 && a.group.value > 0 && a.number <= kFileAreaMaximum && a.revision >= 1 &&
        name_text(a.name) && text_valid(a.description, 256, false) &&
        level(a.permissions.access) && level(a.permissions.upload) && level(a.permissions.download) &&
        static_cast<unsigned>(a.kind) <= static_cast<unsigned>(FileAreaKind::private_sysop) &&
        (!a.sysop_review || a.kind == FileAreaKind::ordinary) &&
        (!a.maximum_upload_bytes || (*a.maximum_upload_bytes >= 1 && *a.maximum_upload_bytes <= kMaximumCallerFileBytes)) &&
        (a.kind != FileAreaKind::review_uploads || a.permissions.downloads_sysop_only) &&
        (a.kind != FileAreaKind::private_sysop || a.permissions.downloads_sysop_only);
}
bool valid_file_count_rule(const FileCountRule& r) noexcept {
    return (!r.daily_downloads || (*r.daily_downloads >= 1 && *r.daily_downloads <= 1000000)) &&
        (!r.ratio || (r.ratio->downloads >= 1 && r.ratio->downloads <= 1000000 &&
                      r.ratio->uploads >= 1 && r.ratio->uploads <= 1000000));
}
bool valid_file_policy(const FilePolicy& p) noexcept {
    return p.revision >= 1 && p.catalog_revision >= 1 && p.maximum_upload_bytes >= 1 &&
        p.maximum_upload_bytes <= kMaximumCallerFileBytes && valid_file_count_rule(p.counts);
}
bool file_area_visible(const FileGroup& g, const FileArea& a, FileAuthority c) noexcept {
    return base_access(g, a, c) && (a.kind != FileAreaKind::private_sysop || c.sysop());
}
bool file_area_uploadable(const FileGroup& g, const FileArea& a, FileAuthority c, bool private_submission) noexcept {
    // Private submission permission does not confer browse access. Group and
    // area enablement, group access and the separate upload minimum still apply.
    const bool private_area=a.kind==FileAreaKind::private_sysop;
    if(!c.valid()||!g.enabled||!a.enabled||!(g.id==a.group)||c.level<g.access||
       c.level<a.permissions.upload||(!a.permissions.uploads_enabled&&!c.sysop()))return false;
    if(private_submission)return private_area;
    return base_access(g,a,c)&&(!private_area||c.sysop());
}
bool file_area_downloadable(const FileGroup& g, const FileArea& a, FileAuthority c) noexcept {
    return file_area_visible(g, a, c) && c.level >= a.permissions.download &&
        (!a.permissions.downloads_sysop_only || c.sysop());
}
std::uint64_t file_upload_limit(const FilePolicy& p, const FileArea& a) noexcept {
    return valid_file_policy(p) && valid_file_area(a) ? a.maximum_upload_bytes.value_or(p.maximum_upload_bytes) : 0;
}
bool valid_file_name(std::string_view s) noexcept {
    if (s.empty() || !text_valid(s,kFileNameBytes,false) || s.front() == ' ' || s.back() == ' ' || s.back() == '.') return false;
    for (unsigned char c : s) if (c < 32 || c == 127 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos) return false;
    auto stem = s.substr(0, s.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
    if (same(stem, "CON") || same(stem, "PRN") || same(stem, "AUX") || same(stem, "NUL") || same(stem, "CLOCK$") ||
        same(stem, "CONIN$") || same(stem, "CONOUT$")) return false;
    if (stem.size() == 4 && (same(stem.substr(0, 3), "COM") || same(stem.substr(0, 3), "LPT")) &&
        stem[3] >= '0' && stem[3] <= '9') return false;
    if (stem.size() == 5 && (same(stem.substr(0, 3), "COM") || same(stem.substr(0, 3), "LPT")) &&
        static_cast<unsigned char>(stem[3]) == 0xc2 &&
        (static_cast<unsigned char>(stem[4]) == 0xb9 || static_cast<unsigned char>(stem[4]) == 0xb2 ||
         static_cast<unsigned char>(stem[4]) == 0xb3)) return false;
    return true;
}
bool valid_file_descriptions(const FileDescriptions& d) noexcept {
    return !d.short_text.empty() && text_valid(d.short_text, kFileShortDescriptionBytes, false) &&
        d.short_text.find_first_not_of(' ') != std::string::npos && text_valid(d.long_text, kFileLongDescriptionBytes, true);
}
std::string sanitize_file_text(std::string_view source, std::size_t maximum, bool multiline) {
    // Legacy DIZ bytes use CP437. Preserve valid UTF-8; never persist a mixture
    // or cut a scalar in half at a byte limit. Terminal control stripping below
    // applies after the lossless legacy conversion, not to raw high CP437 bytes.
    const std::string decoded=valid_file_utf8(source)?std::string(source):qwk_cp437_to_utf8(source);
    source=decoded;
    enum class State { text, escape, csi, string, string_escape };
    State state = State::text;
    std::string out;
    out.reserve(std::min(source.size(), maximum));
    bool last_cr = false;
    for (unsigned char c : source) {
        if (out.size() >= maximum) break;
        if (state == State::escape) {
            if (c == '[') state = State::csi;
            else if (c == ']' || c == 'P' || c == '^' || c == '_') state = State::string;
            else state = State::text;
            continue;
        }
        if (state == State::csi) { if (c >= 0x40 && c <= 0x7e) state = State::text; continue; }
        if (state == State::string) { if (c == 7) state = State::text; else if (c == 27) state = State::string_escape; continue; }
        if (state == State::string_escape) { state = c == '\\' ? State::text : State::string; continue; }
        if (c == 27) { state = State::escape; continue; }
        if (c == '\r' || c == '\n') {
            if (!(c == '\n' && last_cr)) out += multiline ? '\n' : ' ';
            last_cr = c == '\r';
            continue;
        }
        last_cr = false;
        if (c == '\t') out += ' ';
        else if (c >= 32 && c != 127) out += static_cast<char>(c);
    }
    while(!out.empty()&&!valid_file_utf8(out))out.pop_back();
    std::string safe;std::size_t offset{};std::uint32_t scalar{};
    while(offset<out.size()){if(!next_file_scalar(out,offset,scalar))break;if(!unsafe_file_scalar(scalar))append_file_scalar(safe,scalar);}out=std::move(safe);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    const auto first = out.find_first_not_of(" \n");
    return first == std::string::npos ? std::string{} : out.substr(first);
}
std::optional<FileDescriptions> file_diz_descriptions(std::string_view source) {
    // Refuse oversized expanded DIZ instead of treating a truncated description as complete.
    if (source.size() > kFileLongDescriptionBytes) return std::nullopt;
    FileDescriptions result;
    result.long_text = sanitize_file_text(source, kFileLongDescriptionBytes, true);
    result.short_text = sanitize_file_text(result.long_text.substr(0, result.long_text.find('\n')), kFileShortDescriptionBytes, false);
    if (!valid_file_descriptions(result)) return std::nullopt;
    return result;
}
FileDescriptions file_import_descriptions(std::string_view name, std::optional<std::string_view> diz) {
    if (diz) if (auto d = file_diz_descriptions(*diz)) return *d;
    return {sanitize_file_text(name, kFileShortDescriptionBytes, false), {}};
}
bool file_name_matches(std::string_view name, std::string_view query) noexcept {
    if (query.empty() || query.size() > kFileNameBytes || !text_valid(query, kFileNameBytes, false)) return false;
    if (query.find_first_of("*?") == std::string_view::npos) return contains(name, query);
    try{
    std::vector<std::uint32_t> names,pattern;std::size_t at{};std::uint32_t scalar{};
    while(at<name.size()){if(!next_file_scalar(name,at,scalar))return false;names.push_back(fold_file_scalar(scalar));}
    at=0;while(at<query.size()){if(!next_file_scalar(query,at,scalar))return false;pattern.push_back(fold_file_scalar(scalar));}
    std::size_t n = 0, q = 0, star = std::string_view::npos, resume = 0;
    while (n < names.size()) {
        if (q < pattern.size() && (pattern[q] == '?' || pattern[q] == names[n])) { ++q; ++n; }
        else if (q < pattern.size() && pattern[q] == '*') { star = q++; resume = n; }
        else if (star != std::string_view::npos) { q = star + 1; n = ++resume; }
        else return false;
    }
    while (q < pattern.size() && pattern[q] == '*') ++q;
    return q == pattern.size();
    }catch(...){return false;}
}
std::optional<std::vector<std::string>> file_description_terms(std::string_view query) {
    if (query.empty() || query.size() > 512 || !text_valid(query, 512, false)) return std::nullopt;
    std::vector<std::string> terms;
    while (true) {
        const auto delimiter = query.find(';');
        auto term = query.substr(0, delimiter);
        while (!term.empty() && term.front() == ' ') term.remove_prefix(1);
        while (!term.empty() && term.back() == ' ') term.remove_suffix(1);
        if (term.empty() || terms.size() >= 6) return std::nullopt;
        terms.emplace_back(term);
        if (delimiter == std::string_view::npos) break;
        query.remove_prefix(delimiter + 1);
    }
    return terms;
}
bool file_description_matches(const FileDescriptions& d, const std::vector<std::string>& terms) {
    return !terms.empty() && terms.size() <= 6 && std::any_of(terms.begin(), terms.end(), [&](const auto& t) {
        return !t.empty() && (contains(d.short_text, t) || contains(d.long_text, t));
    });
}
bool file_entry_visible(const FileGroup& g, const FileArea& a, const FileEntry& f, FileAuthority c) noexcept {
    return f.area == a.id && f.state == FileCatalogState::available && file_area_visible(g, a, c) && (!f.sysop_only || c.sysop());
}
bool file_description_editable(const FileGroup& g, const FileArea& a, const FileEntry& f, FileAuthority c, AccountId owner) noexcept {
    return file_entry_visible(g, a, f, c) && a.own_description_edit && owner > 0 && f.uploader == owner && f.origin == FileOrigin::caller_upload;
}
bool file_entry_less(const FileEntry& a, const FileEntry& b, FileSort sort) noexcept {
    if (sort == FileSort::newest && a.published_utc != b.published_utc) return a.published_utc > b.published_utc;
    if (sort == FileSort::oldest && a.published_utc != b.published_utc) return a.published_utc < b.published_utc;
    if (sort == FileSort::size && a.bytes != b.bytes) return a.bytes < b.bytes;
    if (sort == FileSort::downloads && a.downloads != b.downloads) return a.downloads > b.downloads;
    const int names = compare_names(a.name, b.name);
    return names ? names < 0 : a.id.value < b.id.value;
}
std::uint64_t file_ratio_entitlement(FileRatio ratio, std::uint64_t uploads) noexcept {
    if (!ratio.downloads || !ratio.uploads) return 0;
    // Divide first: no overflowing product even at the native storage boundary.
    const auto whole = uploads / ratio.uploads, remainder = uploads % ratio.uploads;
    if (whole > UINT64_MAX / ratio.downloads) return UINT64_MAX;
    const auto extra = remainder * static_cast<std::uint64_t>(ratio.downloads) / ratio.uploads;
    return add_saturated(add_saturated(whole * ratio.downloads, extra), ratio.downloads);
}
FileDownloadAllowance file_download_allowance(const FileCountRule& rule, const FileCountState& state,
    bool participating, bool daily_exempt, bool ratio_exempt) noexcept {
    if (!valid_file_count_rule(rule)) return FileDownloadAllowance::invalid_policy;
    if (!daily_exempt && rule.daily_downloads &&
        add_saturated(state.today_downloads, state.reserved_today_downloads) >= *rule.daily_downloads)
        return FileDownloadAllowance::daily_limit;
    if (participating && !ratio_exempt && rule.ratio &&
        add_saturated(state.eligible_downloads, state.reserved_eligible_downloads) >= file_ratio_entitlement(*rule.ratio, state.eligible_uploads))
        return FileDownloadAllowance::ratio_limit;
    return FileDownloadAllowance::allowed;
}
bool FileUploadQueue::add(FileUploadItem item) {
    if (items_.size() >= kFileQueueMaximum || !valid_file_name(item.name) || item.destination.value <= 0 ||
        !valid_file_descriptions(item.descriptions) || std::any_of(items_.begin(), items_.end(), [&](const auto& i) { return same(i.name, item.name); })) return false;
    items_.push_back(std::move(item)); return true;
}
bool FileUploadQueue::remove(std::size_t index) {
    if (index >= items_.size()) return false;
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index)); return true;
}
bool FileDownloadQueue::add(FileId file) {
    if (file.value <= 0 || items_.size() >= kFileQueueMaximum || std::find(items_.begin(), items_.end(), file) != items_.end()) return false;
    items_.push_back(file); return true;
}
bool FileDownloadQueue::remove(FileId file) {
    const auto it = std::find(items_.begin(), items_.end(), file);
    if (it == items_.end()) return false;
    items_.erase(it); return true;
}
IncomingFileAction incoming_file_action(bool enabled, const FileScanResult& result) noexcept {
    if (!enabled && result.verdict == FileScanVerdict::not_scanned) return IncomingFileAction::validate_and_commit;
    if (enabled && result.verdict == FileScanVerdict::clean) return IncomingFileAction::validate_and_commit;
    if (enabled && result.verdict == FileScanVerdict::detected && !result.detections.empty()) return IncomingFileAction::delete_detected_staging;
    return IncomingFileAction::durable_private_hold;
}
std::string file_scan_report(const FileScanResult& result, bool committed, bool deleted) {
    if (result.verdict == FileScanVerdict::detected && !result.detections.empty()) {
        std::string report = deleted ? "Detected - deleted: " : "Detected - held; deletion not confirmed: ";
        for (std::size_t i = 0; i < std::min<std::size_t>(result.detections.size(), 8); ++i) {
            if (i) report += ", ";
            report += sanitize_file_text(result.detections[i], 128, false);
        }
        return report;
    }
    if (committed && result.verdict == FileScanVerdict::clean) return "Passed - added";
    if (committed && result.verdict == FileScanVerdict::not_scanned) return "Added - scanning Off";
    if (result.verdict == FileScanVerdict::clean) return "Scan passed - not added";
    if (result.verdict == FileScanVerdict::not_scanned) return "Not added - scanning Off";
    return "Held privately - scan not completed: " + sanitize_file_text(result.detail, 256, false);
}
} // namespace sfx::application
