#include "sfx/application/message.hpp"

#include <algorithm>
#include <charconv>

namespace sfx::application {
namespace {
bool nonblank(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    });
}
bool control(unsigned char c) { return c < 32 || c == 127; }
std::size_t quote_prefix(std::string_view line) {
    // Short initials and nested > markers are prose quoting. Long indentation,
    // escape sequences and box/block artwork retain their authored layout.
    if(line.find('\x1b')!=line.npos || std::any_of(line.begin(),line.end(),[](unsigned char c){return c>=0xb0&&c<=0xdf;}))return 0;
    std::size_t at{},prefix{};
    while(at<line.size()&&line[at]==' ')++at;
    if(at>3)return 0;
    while(at<line.size()&&at<32){
        const auto start=at;
        while(at<line.size()&&at-start<8&&((line[at]>='a'&&line[at]<='z')||(line[at]>='A'&&line[at]<='Z')||(line[at]>='0'&&line[at]<='9')))++at;
        if(at==line.size()||line[at]!='>')break;
        ++at;while(at<line.size()&&line[at]==' '&&at<32)++at;
        if(at>=32)break;
        prefix=at;
    }
    return prefix;
}
void wrap_line(const std::string& line, std::vector<std::string>& rows) {
    for(auto& row:wrap_message_line(line,kMessageCompositionColumns))rows.push_back(std::move(row.text));
}
std::optional<std::size_t> positive(std::string_view text) {
    if (text.empty()) return {};
    std::size_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !value) return {};
    return value;
}
} // namespace

std::vector<MessageWrappedRow> wrap_message_line(std::string_view source,std::size_t columns,bool preserve_quotes){
    std::vector<MessageWrappedRow> rows;
    if(!columns)return rows;
    const auto candidate=preserve_quotes?quote_prefix(source):0;
    const auto prefix=candidate<columns?candidate:0;
    std::size_t at{};
    do{
        const auto inserted=at?prefix:0;
        const auto available=columns-inserted;
        auto count=std::min(available,source.size()-at);
        if(source.size()-at>available){
            const auto space=source.rfind(' ',at+available-1);
            // A prefix's own spaces cannot create a prefix-only first row.
            if(space!=source.npos&&space>=at&&(!prefix||at||space>=prefix))count=space-at+1;
        }
        MessageWrappedRow row;row.source_begin=at;row.source_size=count;row.inserted_prefix=inserted;
        if(inserted)row.text=source.substr(0,inserted);
        row.text+=source.substr(at,count);rows.push_back(std::move(row));at+=count;
    }while(at<source.size());
    return rows;
}

MessageBody normalize_message_body(std::string_view source, std::uint32_t maximum_lines) {
    MessageBody result;
    const auto fail = [&](MessageValidation why) {
        result.validation = why;
        result.rows.clear();
        result.canonical_lf.clear();
        return result;
    };
    if (!maximum_lines || maximum_lines > kMaximumMessageLines)
        return fail(MessageValidation::invalid_limits);
    if (source.size() > kMessageBodyBytes) return fail(MessageValidation::body_too_large);
    if (source.empty()) return result;
    std::string line;
    std::size_t bytes{};
    bool newline{};
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto c = static_cast<unsigned char>(source[i]);
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < source.size() && source[i + 1] == '\n') ++i;
            wrap_line(line, result.rows);
            line.clear();
            newline = true;
            if (result.rows.size() > maximum_lines) return fail(MessageValidation::too_many_lines);
        } else {
            if (control(c) && c != '\t') return fail(MessageValidation::invalid_control);
            const std::size_t cells = c == '\t' ? 8 - (line.size() % 8) : 1;
            if (cells > kMessageBodyBytes - bytes) return fail(MessageValidation::body_too_large);
            bytes += cells;
            if (c == '\t') line.append(cells, ' ');
            else line.push_back(static_cast<char>(c));
            newline = false;
        }
    }
    // One trailing newline terminates the preceding row. A second is a blank row.
    if (!newline) wrap_line(line, result.rows);
    if (result.rows.size() > maximum_lines) return fail(MessageValidation::too_many_lines);
    for (const auto& row : result.rows) {
        if (row.size() + 1 > kMessageBodyBytes - result.canonical_lf.size())
            return fail(MessageValidation::body_too_large);
        result.canonical_lf += row;
        result.canonical_lf += '\n';
    }
    return result;
}
std::optional<std::size_t> message_body_cursor_offset(std::string_view source,std::uint32_t maximum_lines,std::size_t source_offset){
    if(source_offset>source.size())return {};
    const auto body=normalize_message_body(source,maximum_lines);if(!body)return {};
    if(source.empty())return std::size_t{};
    std::size_t output{},logical_start{},column{},target_column{};bool target{},mapped{};std::size_t result{};
    std::string line;
    const auto flush=[&]{
        const auto rows=wrap_message_line(line,kMessageCompositionColumns);
        for(std::size_t row=0;row<rows.size();++row){
            const auto& current=rows[row];
            if(target && !mapped && target_column<=current.source_begin+current.source_size){
                result=output+current.inserted_prefix+target_column-current.source_begin;
                // At a soft boundary, prefer the start of the following row.
                if(target_column==current.source_begin+current.source_size && row+1<rows.size())result+=1+rows[row+1].inserted_prefix;
                mapped=true;
            }
            output+=current.text.size()+1;
        }
        line.clear();column=0;target=false;
    };
    for(std::size_t i=0;i<source.size();++i){
        if(i==source_offset){target=true;target_column=column;}
        const auto c=static_cast<unsigned char>(source[i]);
        if(c=='\r' || c=='\n'){
            flush();if(c=='\r' && i+1<source.size() && source[i+1]=='\n')++i;logical_start=i+1;
        }else{const auto cells=c=='\t'?8-column%8:1;line.append(cells,c=='\t'?' ':static_cast<char>(c));column+=cells;}
    }
    if(source_offset==source.size()){if(logical_start==source.size())return body.canonical_lf.size();target=true;target_column=column;}
    if(logical_start<source.size())flush();
    return mapped?std::optional<std::size_t>{result}:std::nullopt;
}

MessageValidation validate_message_subject(std::string_view subject, bool /*saving*/,std::size_t maximum) {
    if (maximum>71 || subject.size() > maximum) return MessageValidation::subject_too_long;
    for (unsigned char c : subject) if (control(c)) return MessageValidation::invalid_control;
    return MessageValidation::valid;
}
MessageValidation validate_message_save(std::string_view subject, const MessageBody& body) {
    const auto validation = validate_message_subject(subject, true);
    if (validation != MessageValidation::valid) return validation;
    if (!body) return body.validation;
    if (!nonblank(body.canonical_lf)) return MessageValidation::blank_body;
    return MessageValidation::valid;
}

std::optional<std::vector<std::size_t>> message_quote_selection(
    std::string_view selection, std::size_t source_lines) {
    if (source_lines > kMaximumMessageLines || selection.size() > kMessageBodyBytes) return {};
    std::vector<std::size_t> result;
    std::vector<bool> selected(source_lines, false);
    if (selection == "All" || selection == "ALL" || selection == "all") {
        for (std::size_t i = 0; i < source_lines; ++i) result.push_back(i);
        return result;
    }
    while (!selection.empty()) {
        const auto comma = selection.find(',');
        auto part = selection.substr(0, comma);
        const auto dash = part.find('-');
        const auto first = positive(part.substr(0, dash));
        const auto last = dash == std::string_view::npos ? first : positive(part.substr(dash + 1));
        if (!first || !last || *first > *last || *last > source_lines) return {};
        for (auto i = *first; i <= *last; ++i) selected[i - 1] = true;
        if (comma == std::string_view::npos) break;
        selection.remove_prefix(comma + 1);
        if (selection.empty()) return {};
    }
    for (std::size_t i = 0; i < selected.size(); ++i) if (selected[i]) result.push_back(i);
    if (result.empty()) return {};
    return result;
}

MessageDraft::MessageDraft(std::uint32_t maximum_lines) : maximum_lines_(maximum_lines) {
    body_ = normalize_message_body({}, maximum_lines);
}
MessageValidation MessageDraft::set_subject(std::string_view subject) {
    const auto result = validate_message_subject(subject, false);
    if (result == MessageValidation::valid) subject_.assign(subject);
    return result;
}
MessageValidation MessageDraft::replace_body(std::string_view source) {
    auto body = normalize_message_body(source, maximum_lines_);
    const auto result = body.validation;
    if (body) body_ = std::move(body);
    return result;
}
MessageValidation MessageDraft::insert_text(std::size_t offset, std::string_view data) {
    if (offset > body_.canonical_lf.size()) return MessageValidation::invalid_selection;
    if (data.size() > kMessageBodyBytes - body_.canonical_lf.size())
        return MessageValidation::body_too_large;
    auto merged = body_.canonical_lf;
    merged.insert(offset, data);
    return replace_body(merged);
}
MessageValidation MessageDraft::insert_quote(std::size_t offset,
    const std::vector<std::string>& source, const std::vector<std::size_t>& selection) {
    if (offset > body_.canonical_lf.size() || selection.empty()) return MessageValidation::invalid_selection;
    auto indices = selection;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    std::string quote;
    for (const auto index : indices) {
        if (index >= source.size()) return MessageValidation::invalid_selection;
        // A source row is data, not a command stream or multiple injected rows.
        for (unsigned char c : source[index]) if (control(c)) return MessageValidation::invalid_control;
        if (quote.size() > kMessageBodyBytes - 3 ||
            source[index].size() > kMessageBodyBytes - 3 - quote.size())
            return MessageValidation::body_too_large;
        quote += "> ";
        quote += source[index];
        quote += '\n';
    }
    return insert_text(offset, quote);
}
MessageValidation MessageDraft::save_validation() const {
    return validate_message_save(subject_, body_);
}
MessageValidation MessageDraft::replace_line(std::size_t line,std::string_view text) {
    if(!line || line>body_.rows.size())return MessageValidation::invalid_selection;
    // A replacement prompt treats slash commands as data, but not embedded rows.
    if(text.find_first_of("\r\n")!=std::string_view::npos)return MessageValidation::invalid_control;
    std::string merged;for(std::size_t i=0;i<body_.rows.size();++i){merged+=i+1==line?std::string(text):body_.rows[i];merged+='\n';}
    return replace_body(merged);
}
MessageValidation MessageDraft::insert_line(std::size_t line,std::string_view text) {
    if(!line || line>body_.rows.size()+1)return MessageValidation::invalid_selection;
    if(text.find_first_of("\r\n")!=std::string_view::npos)return MessageValidation::invalid_control;
    std::string merged;for(std::size_t i=0;i<=body_.rows.size();++i){if(i+1==line){merged+=text;merged+='\n';}if(i<body_.rows.size()){merged+=body_.rows[i];merged+='\n';}}
    return replace_body(merged);
}
MessageValidation MessageDraft::delete_line(std::size_t line) {
    if(!line || line>body_.rows.size())return MessageValidation::invalid_selection;
    std::string merged;for(std::size_t i=0;i<body_.rows.size();++i)if(i+1!=line){merged+=body_.rows[i];merged+='\n';}
    return replace_body(merged);
}
MessageValidation MessageDraft::erase_text(std::size_t offset,std::size_t count) {
    if(offset>body_.canonical_lf.size() || count>body_.canonical_lf.size()-offset)return MessageValidation::invalid_selection;
    auto merged=body_.canonical_lf;merged.erase(offset,count);return replace_body(merged);
}
MessageValidation MessageDraft::change_limit(std::uint32_t maximum_lines) {
    const auto normalized=normalize_message_body(body_.canonical_lf,maximum_lines);
    if(!normalized)return normalized.validation;
    maximum_lines_=maximum_lines;return MessageValidation::valid;
}
bool valid_message_retention_policy(const MessageRetentionPolicy& policy) noexcept {
    return (!policy.maximum_count || (*policy.maximum_count >= 1 && *policy.maximum_count <= kMaximumRetainedMessageCount)) &&
           (!policy.maximum_age_days || (*policy.maximum_age_days >= 1 && *policy.maximum_age_days <= kMaximumMessageAgeDays));
}
ParsedMessageRetentionLimit parse_message_retention_limit(std::string_view text, std::uint32_t maximum) noexcept {
    constexpr std::string_view unlimited = "Unlimited";
    if (text.size() == unlimited.size()) {
        bool same = true;
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto a = text[i], b = unlimited[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
            same = same && a == b;
        }
        if (same) return {true, {}};
    }
    if (text.empty() || text.size() > 10 || maximum == 0) return {};
    std::uint32_t value{};
    for (const auto digit : text) {
        if (digit < '0' || digit > '9') return {};
        const auto next = static_cast<std::uint32_t>(digit - '0');
        if (next > maximum || value > (maximum - next) / 10) return {};
        value = value * 10 + next;
    }
    return value >= 1 && value <= maximum ? ParsedMessageRetentionLimit{true, value} : ParsedMessageRetentionLimit{};
}
bool message_age_expired(std::int64_t arrival, std::int64_t now,
                         std::optional<std::uint32_t> days) noexcept {
    if (!days || *days < 1 || *days > kMaximumMessageAgeDays ||
        !valid_utc_seconds(arrival) || !valid_utc_seconds(now) || now < arrival) return false;
    return now - arrival >= static_cast<std::int64_t>(*days) * 86400;
}
} // namespace sfx::application
