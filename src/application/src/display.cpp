#include "sfx/application/display.hpp"
#include "sfx/application/menu.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <iterator>
#include <utility>

namespace sfx::application {

std::string_view trusted_menu_basename(const TrustedDisplayIdentity identity) noexcept {
    switch (identity) {
        case TrustedDisplayIdentity::menu_main: return "MAIN";
        case TrustedDisplayIdentity::menu_messages: return "MSG";
        case TrustedDisplayIdentity::menu_files: return "FILE";
        case TrustedDisplayIdentity::menu_batch_transfer: return "BATCH";
        case TrustedDisplayIdentity::menu_qwk: return "QWK";
        case TrustedDisplayIdentity::menu_sysop: return "SYSOP";
        case TrustedDisplayIdentity::menu_doors: return "DOOR";
        case TrustedDisplayIdentity::menu_bulletins: return "BULLETIN";
        case TrustedDisplayIdentity::menu_door_bulletins: return "DBUL";
        case TrustedDisplayIdentity::menu_chat: return "CHAT";
        case TrustedDisplayIdentity::menu_settings: return "YOUR";
        case TrustedDisplayIdentity::menu_file_groups: return "FGRP";
        case TrustedDisplayIdentity::menu_file_areas: return "FAREA";
        default: return {};
    }
}

namespace {

constexpr std::array<std::string_view, 68> kValueTokens{
    "FNAME", "NAME", "ALIAS", "EMAIL", "PHONENUM", "CITYSTATE", "SLEVEL", "LOGONS", "ORGLOG",
    "LASTCALL", "LOGTIME", "TIMEON", "TIMELEFT", "SUBDATE", "PROTOCOL", "QUIET", "HOTKEY", "UPLOADS",
    "DOWNLOADS", "UPK", "DOWNK", "POSTS", "IPADDR", "HOSTNAME", "PWAGE", "MAILTOTAL", "MAILUNREAD",
    "BBSNAME", "SYSOP", "VERSION", "DATE", "TIME", "DATETIME", "NODE", "NODES", "ONLINE", "TRANSPORT",
    "UPTIME", "GRAPHICS", "COLS", "ROWS", "MSGCONF", "MSGCONFNUM", "MSGCOUNT", "NEWMSGS", "MSGNUM",
    "MSGTO", "MSGFROM", "MSGSUBJECT", "MSGDATE", "FILEAREA", "FILEAREANUM", "FILECOUNT", "NEWFILES",
    "FILENAME", "FILEDESC", "FILESIZE", "FILEUPLOADER", "FILEDATE", "FILEDLS", "DOORNAME", "DOORNUM",
    "DOORMENU", "CHATROOM", "CHATCOUNT", "FTNNET", "FTNADDR"};

constexpr std::array<std::string_view, 7> kControlTokens{
    "PROMPTOFF", "NOABORT", "BEEP", "CLS", "ABORTON", "PROMPT", "PAUSE"};

constexpr std::array<std::string_view, 12> kNamedFormats{
    "HMS", "MIN", "SHORT", "AUTO", "B", "K", "M", "G", "LONG", "12", "24", ""};

constexpr std::array<std::string_view, 11> kPreauthenticationTokens{
    "BBSNAME", "SYSOP", "VERSION", "DATE", "TIME", "DATETIME",
    "NODE", "NODES", "TRANSPORT", "COLS", "ROWS"};

constexpr std::array<std::string_view, 14> kAuthenticatedTokens{
    "FNAME", "CITYSTATE", "PHONENUM", "NAME", "ALIAS", "EMAIL",
    "SLEVEL", "LOGONS", "LASTCALL", "TIMEON", "PROTOCOL", "IPADDR",
    "PWAGE", "GRAPHICS"};

constexpr std::array<std::string_view, 23> kReservedDeviceBasenames{
    "CON", "PRN", "AUX", "NUL", "CLOCK$",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

std::string upper(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

bool all_basename_characters(const std::string_view basename) {
    return !basename.empty() && std::all_of(basename.begin(), basename.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_';
    });
}

constexpr unsigned char ascii_upper(
    const unsigned char character) noexcept {
    return character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')
        ? static_cast<unsigned char>(
              character - static_cast<unsigned char>('a') +
              static_cast<unsigned char>('A'))
        : character;
}

bool ascii_case_equal(
    const std::string_view left,
    const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_upper(static_cast<unsigned char>(left[index])) !=
            ascii_upper(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

void append_literal_segment(
    DisplayParseResult& result,
    std::string_view literal) {
    if (literal.empty()) return;
    if (!result.segments.empty() &&
        result.segments.back().kind == DisplaySegmentKind::literal) {
        result.segments.back().literal.append(literal.data(), literal.size());
    } else {
        result.segments.push_back(
            {DisplaySegmentKind::literal, std::string(literal), std::nullopt});
    }
}

}  // namespace

bool DisplayTokenRegistry::is_known_value(const std::string_view name) const noexcept {
    return std::find(kValueTokens.begin(), kValueTokens.end(), name) != kValueTokens.end();
}

bool DisplayTokenRegistry::is_known_control(const std::string_view name) const noexcept {
    return std::find(kControlTokens.begin(), kControlTokens.end(), name) != kControlTokens.end();
}

std::optional<ParsedDisplayToken> DisplayTokenRegistry::parse_expression(const std::string_view expression) const {
    const auto normalized = upper(expression);
    if (normalized.rfind("INCLUDE:", 0) == 0) {
        const auto filename = std::string(expression.substr(8));
        if (!valid_include_filename(filename)) {
            return std::nullopt;
        }
        return ParsedDisplayToken{DisplayTokenKind::include, "INCLUDE", filename, {}};
    }

    const auto separator = normalized.find_first_of("|:");
    const auto name = normalized.substr(0, separator);
    if (name == "PASSWORD" || name == "BIRTHDATE") {
        return std::nullopt;
    }
    if (is_known_control(name)) {
        if (separator != std::string::npos) {
            return std::nullopt;
        }
        return ParsedDisplayToken{DisplayTokenKind::control, name, {}, {}};
    }
    if (!is_known_value(name)) {
        return std::nullopt;
    }

    TokenFormat format;
    if (separator != std::string::npos) {
        const auto modifier = normalized.substr(separator + 1);
        if (normalized[separator] == '|') {
            if (modifier.size() < 2) {
                return std::nullopt;
            }
            switch (modifier.front()) {
            case 'L': format.alignment = FieldAlignment::left; break;
            case 'R': format.alignment = FieldAlignment::right; break;
            case 'C': format.alignment = FieldAlignment::center; break;
            case 'Z': format.alignment = FieldAlignment::zero_pad; break;
            default: return std::nullopt;
            }
            std::uint32_t width = 0;
            for (const char character : modifier.substr(1)) {
                if (character < '0' || character > '9') {
                    return std::nullopt;
                }
                width = width * 10U + static_cast<std::uint32_t>(character - '0');
                if (width > kMaximumDisplayFieldWidth) return std::nullopt;
            }
            if (width == 0 || width > kMaximumDisplayFieldWidth) {
                return std::nullopt;
            }
            format.width = static_cast<std::uint16_t>(width);
        } else {
            if (std::find(kNamedFormats.begin(), kNamedFormats.end(), modifier) == kNamedFormats.end()) {
                return std::nullopt;
            }
            format.named_format = modifier;
        }
    }
    return ParsedDisplayToken{DisplayTokenKind::value, name, {}, format};
}

DisplayParseResult parse_display(
    const std::string_view source,
    const DisplayTrust trust,
    const DisplayTokenRegistry& registry) {
    DisplayParseResult result;
    if (trust == DisplayTrust::untrusted) {
        result.segments.push_back({DisplaySegmentKind::literal, std::string(source), std::nullopt});
        return result;
    }

    std::size_t position = 0;
    while (position < source.size()) {
        const auto open = source.find('@', position);
        if (open == std::string_view::npos) {
            append_literal_segment(result, source.substr(position));
            break;
        }
        if (open > position) {
            append_literal_segment(
                result, source.substr(position, open - position));
        }
        if (open + 1U < source.size() && source[open + 1U] == '@') {
            append_literal_segment(result, "@");
            position = open + 2U;
            continue;
        }
        // A standalone menu key ("@ Sysop Utilities", including ANSI styling
        // immediately after @) is text, not an incomplete display code.
        if(open+1U==source.size() || !((source[open+1]>='A' && source[open+1]<='Z') ||
            (source[open+1]>='a' && source[open+1]<='z') || source[open+1]=='_')) {
            append_literal_segment(result,"@");position=open+1;continue;
        }
        // Email addresses in owner-authored CP437 text are literal. Do not
        // relax @TOKEN@ validation, including forbidden secret tokens.
        const auto address_char=[](char c) {return (c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
            (c>='0'&&c<='9') || c=='.' || c=='-' || c=='_';};
        if(open>0 && address_char(source[open-1])) {
            auto end=open+1;
            while(end<source.size() && end-open<=254U && address_char(source[end])) ++end;
            const auto domain=source.substr(open+1,end-open-1);
            const auto dot=domain.find('.');
            if(dot!=std::string_view::npos && dot>0 && dot+1<domain.size()) {
                append_literal_segment(result,"@");position=open+1;continue;
            }
        }
        const auto close = source.find('@', open + 1);
        if (close == std::string_view::npos) {
            result.errors.push_back("unterminated_token");
            break;
        }
        const auto expression = source.substr(open + 1, close - open - 1);
        const auto token = registry.parse_expression(expression);
        if (!token.has_value()) {
            result.errors.push_back("unknown_or_forbidden_token:" + std::string(expression));
        } else {
            result.segments.push_back({DisplaySegmentKind::token, {}, token});
        }
        position = close + 1;
    }
    if (source.empty()) {
        result.segments.push_back({DisplaySegmentKind::literal, {}, std::nullopt});
    }
    return result;
}

bool valid_include_filename(const std::string_view filename) noexcept {
    if (filename.empty() || filename.size() > 255U ||
        filename.front() == ' ' || filename.back() == ' ' ||
        filename.back() == '.' ||
        filename.find("..") != std::string_view::npos ||
        filename.find('/') != std::string_view::npos || filename.find('\\') != std::string_view::npos ||
        filename.find(':') != std::string_view::npos ||
        filename.find('*') != std::string_view::npos ||
        filename.find('?') != std::string_view::npos) {
        return false;
    }
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    const auto basename = filename.substr(0, dot);
    const auto extension = filename.substr(dot);
    return (ascii_case_equal(extension, ".ANS") ||
               ascii_case_equal(extension, ".ASC")) &&
        all_basename_characters(basename) &&
        std::none_of(kReservedDeviceBasenames.begin(),
            kReservedDeviceBasenames.end(),
            [basename](const std::string_view reserved) {
                return ascii_case_equal(basename, reserved);
            });
}

std::optional<DisplayLookupPlan> make_display_lookup_plan(
    const std::string_view basename,
    const GraphicsMode graphics,
    const core::SecurityLevel security_level) {
    if (!all_basename_characters(basename)) {
        return std::nullopt;
    }
    const auto normalized = upper(basename);
    const std::string extension = graphics == GraphicsMode::ansi ? ".ANS" : ".ASC";
    return DisplayLookupPlan{
        normalized + std::to_string(security_level.value()) + extension,
        normalized + extension};
}

namespace {

bool safe_context_value(const std::string_view value) noexcept {
    if (value.size() > kMaximumDisplayFieldWidth * 8U) return false;
    for (const unsigned char character : value) {
        if (character < 0x20U || character == 0x7fU) return false;
    }
    return true;
}

bool eligible_value_token(
    const TrustedDisplayIdentity identity,
    const std::string_view name) {
    if (!trusted_menu_basename(identity).empty() || identity==TrustedDisplayIdentity::door_bulletin_page) {
        return std::find(kValueTokens.begin(), kValueTokens.end(), name) !=
            kValueTokens.end();
    }
    if (std::find(kPreauthenticationTokens.begin(),
            kPreauthenticationTokens.end(), name) !=
        kPreauthenticationTokens.end()) {
        return true;
    }
    if ((identity == TrustedDisplayIdentity::welcome1 ||
            identity == TrustedDisplayIdentity::newreg ||
            identity == TrustedDisplayIdentity::newuser) &&
        name == "GRAPHICS") {
        return true;
    }
    return identity == TrustedDisplayIdentity::newuser &&
        std::find(kAuthenticatedTokens.begin(), kAuthenticatedTokens.end(),
            name) != kAuthenticatedTokens.end();
}

bool context_value(
    const TrustedDisplaySelectionRequest& request,
    const std::string_view name,
    std::string& value) {
    const auto required = [&](const std::optional<std::string>& candidate) {
        if (!candidate.has_value() || candidate->empty() ||
            !safe_context_value(*candidate)) {
            return false;
        }
        value = *candidate;
        return true;
    };
    const auto optional = [&](const std::optional<std::string>& candidate) {
        if (candidate.has_value()) {
            if (!safe_context_value(*candidate)) return false;
            value = *candidate;
        } else {
            value.clear();
        }
        return true;
    };

    const auto& context = request.context;
    if (name == "BBSNAME") return required(context.bbs_name);
    if (name == "SYSOP") return required(context.sysop);
    if (name == "VERSION") return required(context.version);
    if (name == "DATE") return required(context.date);
    if (name == "TIME") return required(context.time);
    if (name == "DATETIME") return required(context.datetime);
    if (name == "NODE") return required(context.node);
    if (name == "NODES") return required(context.nodes);
    if (name == "TRANSPORT") return required(context.transport);
    if (name == "COLS") return required(context.columns);
    if (name == "ROWS") return required(context.rows);
    if (name == "GRAPHICS") {
        value = request.graphics == GraphicsMode::ansi ? "ANSI" : "ASCII";
        return true;
    }
    if (name == "FNAME") return required(context.first_name);
    if (name == "CITYSTATE") return optional(context.city_state);
    if (name == "PHONENUM") return optional(context.phone_number);
    if (name == "NAME") return required(context.name);
    if (name == "ALIAS") return required(context.alias);
    if (name == "EMAIL") return optional(context.email);
    if (name == "SLEVEL") return required(context.security_level);
    if (name == "LOGONS") return required(context.logons);
    if (name == "LASTCALL") return optional(context.last_call);
    if (name == "TIMEON") return required(context.time_on);
    if (name == "PROTOCOL") return required(context.protocol);
    if (name == "IPADDR") return required(context.ip_address);
    if (name == "PWAGE") return optional(context.password_age);
    if (!trusted_menu_basename(request.identity).empty() || request.identity==TrustedDisplayIdentity::door_bulletin_page) {
        if (name == "ORGLOG") return optional(context.original_logon);
        if (name == "LOGTIME") return optional(context.logon_minutes);
        if (name == "TIMELEFT") return optional(context.time_left);
        if (name == "SUBDATE") return optional(context.subscription_days);
        if (name == "QUIET") return optional(context.quiet);
        if (name == "HOTKEY") return optional(context.hotkey);
        if (name == "UPLOADS") return optional(context.uploads);
        if (name == "DOWNLOADS") return optional(context.downloads);
        if (name == "UPK") return optional(context.uploaded_kb);
        if (name == "DOWNK") return optional(context.downloaded_kb);
        if (name == "POSTS") return optional(context.posts);
        if (name == "HOSTNAME") return optional(context.hostname);
        if (name == "MAILTOTAL") return optional(context.mail_total);
        if (name == "MAILUNREAD") return optional(context.mail_unread);
        if (name == "ONLINE") return optional(context.online);
        if (name == "UPTIME") return optional(context.uptime);
        if (name == "MSGCONF") return optional(context.message_conference);
        if (name == "MSGCONFNUM") return optional(context.message_conference_number);
        if (name == "MSGCOUNT") return optional(context.message_count);
        if (name == "NEWMSGS") return optional(context.new_messages);
        if (name == "MSGNUM") return optional(context.message_number);
        if (name == "MSGTO") return optional(context.message_to);
        if (name == "MSGFROM") return optional(context.message_from);
        if (name == "MSGSUBJECT") return optional(context.message_subject);
        if (name == "MSGDATE") return optional(context.message_date);
        if (name == "FILEAREA") return optional(context.file_area);
        if (name == "FILEAREANUM") return optional(context.file_area_number);
        if (name == "FILECOUNT") return optional(context.file_count);
        if (name == "NEWFILES") return optional(context.new_files);
        if (name == "FILENAME") return optional(context.filename);
        if (name == "FILEDESC") return optional(context.file_description);
        if (name == "FILESIZE") return optional(context.file_size);
        if (name == "FILEUPLOADER") return optional(context.file_uploader);
        if (name == "FILEDATE") return optional(context.file_date);
        if (name == "FILEDLS") return optional(context.file_downloads);
        if (name == "DOORNAME") return optional(context.door_name);
        if (name == "DOORNUM") return optional(context.door_number);
        if (name == "DOORMENU") return optional(context.door_menu);
        if (name == "CHATROOM") return optional(context.chat_room);
        if (name == "CHATCOUNT") return optional(context.chat_count);
        if (name == "FTNNET") return optional(context.ftn_network);
        if (name == "FTNADDR") return optional(context.ftn_address);
    }
    return false;
}

bool parse_unsigned_decimal(
    const std::string_view value,
    std::uint64_t& parsed) noexcept {
    if (value.empty()) return false;
    parsed = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - '0');
        if (parsed >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    return true;
}

std::string two_digits(const std::uint64_t value) {
    const std::string raw = std::to_string(value);
    return raw.size() >= 2U ? raw : "0" + raw;
}

bool format_menu_value(const ParsedDisplayToken& token, std::string& value) {
    const auto& format = token.format.named_format;
    if (format.empty() || value.empty()) return true;
    const auto& name = token.name;
    const bool duration = name == "TIMEON" || name == "TIMELEFT" || name == "UPTIME";
    if (duration) {
        if (format != "HMS" && format != "MIN" && format != "SHORT") return false;
        if (value == "Unlimited") return true;
        std::uint64_t seconds = 0;
        if (!parse_unsigned_decimal(value, seconds)) return false;
        if (format == "MIN") value = std::to_string(seconds / 60U);
        else if (format == "HMS") value = std::to_string(seconds / 3600U) + ":" +
            two_digits((seconds / 60U) % 60U) + ":" + two_digits(seconds % 60U);
        else if (seconds >= 3600U) value = std::to_string(seconds / 3600U) + "h " +
            std::to_string((seconds / 60U) % 60U) + "m";
        else value = std::to_string(seconds / 60U) + "m " + std::to_string(seconds % 60U) + "s";
        return true;
    }
    if (name == "FILESIZE") {
        if (format != "AUTO" && format != "B" && format != "K" && format != "M" && format != "G")
            return false;
        std::uint64_t bytes = 0;
        if (!parse_unsigned_decimal(value, bytes)) return false;
        std::uint64_t divisor = 1;
        std::string suffix = " B";
        if (format == "G" || (format == "AUTO" && bytes >= 1073741824ULL)) {
            divisor = 1073741824ULL; suffix = " G";
        } else if (format == "M" || (format == "AUTO" && bytes >= 1048576U)) {
            divisor = 1048576U; suffix = " M";
        } else if (format == "K" || (format == "AUTO" && bytes >= 1024U)) {
            divisor = 1024U; suffix = " K";
        }
        value = std::to_string(bytes / divisor) + suffix;
        return true;
    }
    if (name == "DATE" || name == "ORGLOG" || name == "LASTCALL" || name == "DATETIME" ||
        name == "MSGDATE" || name == "FILEDATE") {
        if (format != "SHORT" && format != "LONG") return false;
        // Canonical BBS-local calendar date; conversion never consults the host
        // locale or interprets a caller-controlled formatting expression.
        if (value.size() < 10U || value[4] != '-' || value[7] != '-') return false;
        std::uint64_t year = 0, month = 0, day = 0;
        if (!parse_unsigned_decimal(std::string_view(value).substr(0,4), year) ||
            !parse_unsigned_decimal(std::string_view(value).substr(5,2), month) ||
            !parse_unsigned_decimal(std::string_view(value).substr(8,2), day) ||
            year == 0 || month == 0 || month > 12) return false;
        constexpr std::array<unsigned,12> days{{31,28,31,30,31,30,31,31,30,31,30,31}};
        const bool leap = year % 4U == 0 && (year % 100U != 0 || year % 400U == 0);
        const auto month_index = static_cast<std::size_t>(month-1U);
        if (day == 0 || day > days[month_index] + (month == 2U && leap ? 1U : 0U)) return false;
        const auto tail = value.substr(10);
        if (!tail.empty() && (tail.size() != 9U || tail[0] != ' ' || tail[3] != ':' || tail[6] != ':'))
            return false;
        if (!tail.empty()) {
            std::uint64_t hour=0, minute=0, second=0;
            if (!parse_unsigned_decimal(std::string_view(tail).substr(1,2),hour) ||
                !parse_unsigned_decimal(std::string_view(tail).substr(4,2),minute) ||
                !parse_unsigned_decimal(std::string_view(tail).substr(7,2),second) ||
                hour>23 || minute>59 || second>59) return false;
        }
        if (format == "LONG") {
            constexpr std::array<std::string_view,12> months{{"January","February","March","April",
                "May","June","July","August","September","October","November","December"}};
            value = std::string(months[month_index]) + " " + std::to_string(day) + ", " +
                std::to_string(year) + tail;
        }
        return true;
    }
    if (name == "TIME") {
        if (format != "12" && format != "24") return false;
        if (value.size() != 8U || value[2] != ':' || value[5] != ':') return false;
        std::uint64_t hour = 0, minute = 0, second = 0;
        if (!parse_unsigned_decimal(std::string_view(value).substr(0,2), hour) ||
            !parse_unsigned_decimal(std::string_view(value).substr(3,2), minute) ||
            !parse_unsigned_decimal(std::string_view(value).substr(6,2), second) ||
            hour > 23U || minute > 59U || second > 59U) return false;
        if (format == "12") value = std::to_string(hour % 12U == 0 ? 12U : hour % 12U) +
            value.substr(2) + (hour >= 12U ? " PM" : " AM");
        return true;
    }
    return false;
}

std::optional<std::string_view> menu_asset_content(const std::string_view bytes) {
    if (bytes.size() < 128U || bytes.substr(bytes.size()-128U,7U) != "SAUCE00") {
        // One DOS SUB at physical EOF is compatible; embedded/repeated SUB
        // still fails literal validation. Never rewrite the source snapshot.
        if (!bytes.empty() && static_cast<unsigned char>(bytes.back()) == 0x1aU)
            return bytes.substr(0, bytes.size()-1U);
        return bytes;
    }
    const auto metadata = bytes.size()-128U;
    const auto byte = [&](std::size_t n) { return static_cast<unsigned char>(bytes[metadata+n]); };
    const std::size_t size = byte(90) | (static_cast<std::size_t>(byte(91))<<8U) |
        (static_cast<std::size_t>(byte(92))<<16U) | (static_cast<std::size_t>(byte(93))<<24U);
    // Character artwork only, with bounded optional SAUCE comment records.
    if (byte(94) != 1U || (byte(95) != 0U && byte(95) != 1U) || size > metadata) return std::nullopt;
    std::size_t trailer = size;
    if (trailer < metadata && static_cast<unsigned char>(bytes[trailer]) == 0x1aU) ++trailer;
    const auto comments = byte(104);
    if (comments) {
        if (metadata-trailer != 5U + static_cast<std::size_t>(comments)*64U ||
            bytes.substr(trailer,5U) != "COMNT") return std::nullopt;
        trailer = metadata;
    }
    if (trailer != metadata) return std::nullopt;
    return bytes.substr(0,size);
}

bool apply_named_format(
    const std::string_view named_format,
    std::string& value) {
    if (named_format.empty() || named_format == "SHORT" ||
        named_format == "LONG" || named_format == "12" ||
        named_format == "24") {
        return true;
    }

    std::uint64_t number = 0;
    if (!parse_unsigned_decimal(value, number)) return false;
    if (named_format == "HMS") {
        const auto hours = number / 3600U;
        const auto minutes = (number / 60U) % 60U;
        const auto seconds = number % 60U;
        value = std::to_string(hours) + ":" + two_digits(minutes) +
            ":" + two_digits(seconds);
        return true;
    }
    if (named_format == "MIN") {
        value = std::to_string(number / 60U);
        return true;
    }

    std::uint64_t divisor = 1;
    std::string suffix = " B";
    if (named_format == "K") {
        divisor = 1024U;
        suffix = " K";
    } else if (named_format == "M") {
        divisor = 1024U * 1024U;
        suffix = " M";
    } else if (named_format == "G") {
        divisor = 1024U * 1024U * 1024U;
        suffix = " G";
    } else if (named_format == "AUTO") {
        if (number >= 1024ULL * 1024ULL * 1024ULL) {
            divisor = 1024ULL * 1024ULL * 1024ULL;
            suffix = " G";
        } else if (number >= 1024ULL * 1024ULL) {
            divisor = 1024ULL * 1024ULL;
            suffix = " M";
        } else if (number >= 1024ULL) {
            divisor = 1024ULL;
            suffix = " K";
        }
    } else if (named_format != "B") {
        return false;
    }
    value = std::to_string(number / divisor) + suffix;
    return true;
}

bool apply_field_format(
    const TokenFormat& format,
    std::string& value) {
    if (!apply_named_format(format.named_format, value)) return false;
    if (format.alignment == FieldAlignment::none) {
        return true;
    }
    if (format.alignment == FieldAlignment::zero_pad) {
        std::uint64_t numeric = 0;
        if (!parse_unsigned_decimal(value, numeric)) return false;
        static_cast<void>(numeric);
    }
    if (value.size() > format.width) {
        switch (format.alignment) {
            case FieldAlignment::left:
                value.resize(format.width);
                return true;
            case FieldAlignment::right:
            case FieldAlignment::zero_pad:
                value.erase(0, value.size() - format.width);
                return true;
            case FieldAlignment::center: {
                const std::size_t start =
                    (value.size() - format.width) / 2U;
                value = value.substr(start, format.width);
                return true;
            }
            case FieldAlignment::none:
                return true;
        }
    }
    if (value.size() == format.width) return true;
    const std::size_t padding = format.width - value.size();
    switch (format.alignment) {
        case FieldAlignment::left:
            value.append(padding, ' ');
            return true;
        case FieldAlignment::right:
            value.insert(0, padding, ' ');
            return true;
        case FieldAlignment::center: {
            const std::size_t left = padding / 2U;
            value.insert(0, left, ' ');
            value.append(padding - left, ' ');
            return true;
        }
        case FieldAlignment::zero_pad:
            value.insert(0, padding, '0');
            return true;
        case FieldAlignment::none:
            return true;
    }
    return false;
}

std::size_t control_output_bytes(
    const TerminalDisplayOperationKind kind) noexcept {
    switch (kind) {
        case TerminalDisplayOperationKind::logical_newline:
        case TerminalDisplayOperationKind::paging_newline: return 2U;
        case TerminalDisplayOperationKind::clear_screen: return 7U;
        case TerminalDisplayOperationKind::caller_bell: return 1U;
        case TerminalDisplayOperationKind::pause: return 2U;
        default: return 0U;
    }
}

std::optional<std::size_t> safe_ansi_sequence_length(
    const std::string_view source,
    const std::size_t offset) {
    if (offset >= source.size() ||
        static_cast<unsigned char>(source[offset]) != 0x1bU ||
        offset + 1U >= source.size()) {
        return std::nullopt;
    }
    if (source[offset + 1U] != '[') {
        const std::string_view candidate = source.substr(offset, 2U);
        return valid_terminal_ansi_sequence(candidate)
            ? std::optional<std::size_t>{2U}
            : std::nullopt;
    }
    const std::size_t maximum =
        std::min(source.size(), offset + static_cast<std::size_t>(40U));
    for (std::size_t end = offset + 2U; end < maximum; ++end) {
        const auto character = static_cast<unsigned char>(source[end]);
        if (character >= 0x40U && character <= 0x7eU) {
            const auto length = end - offset + 1U;
            const auto candidate = source.substr(offset, length);
            return valid_terminal_ansi_sequence(candidate)
                ? std::optional<std::size_t>{length}
                : std::nullopt;
        }
        if (character < 0x30U || character > 0x3fU) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

class TrustedDisplayRenderer final {
public:
    TrustedDisplayRenderer(
        ITrustedDisplayAssetReader& reader,
        const DisplayTokenRegistry& registry,
        const TrustedDisplaySelectionRequest& request) noexcept
        : reader_(reader), registry_(registry), request_(request) {}

    std::optional<RenderedTrustedDisplayProgram> render(
        const TrustedDisplayAssetReadResult& root,
        const bool exact_security_match) {
        output_ = {};
        text_column_modulo_ = 0;
        aggregate_source_bytes_ = 0;
        include_count_ = 0;
        logical_stack_.clear();
        file_stack_.clear();
        if (!render_asset(root, 0U)) return std::nullopt;
        if (output_.operations.empty() ||
            !valid_terminal_display_program(output_)) {
            return std::nullopt;
        }

        RenderedTrustedDisplayProgram program;
        program.identity = request_.identity;
        program.door_asset_number = request_.door_asset_number;
        program.graphics = request_.graphics;
        program.selected_logical_asset = upper(root.logical_identity);
        program.exact_security_match = exact_security_match;
        program.terminal_program = std::move(output_);
        program.aggregate_source_bytes = aggregate_source_bytes_;
        program.resolved_include_count = include_count_;
        program.modified_utc_100ns = root.modified_utc_100ns;
        return valid_rendered_trusted_display_program(program)
            ? std::optional<RenderedTrustedDisplayProgram>{
                  std::move(program)}
            : std::nullopt;
    }

private:
    bool add_operation(
        const TerminalDisplayOperationKind kind,
        std::string bytes = {}) {
        const std::size_t addition = bytes.empty()
            ? control_output_bytes(kind)
            : bytes.size();
        if (addition >
            kMaximumRenderedDisplayBytes - output_.output_byte_count) {
            return false;
        }
        if (kind == TerminalDisplayOperationKind::cp437_text &&
            !output_.operations.empty() &&
            output_.operations.back().kind == kind &&
            output_.operations.back().playback_bps == playback_bps_) {
            if (bytes.size() >
                kMaximumRenderedDisplayBytes -
                    output_.operations.back().bytes.size()) {
                return false;
            }
            output_.operations.back().bytes += bytes;
        } else {
            if (output_.operations.size() >=
                kTerminalMaximumDisplayOperations) {
                return false;
            }
            output_.operations.push_back({kind, std::move(bytes), playback_bps_});
        }
        output_.output_byte_count += addition;
        if(kind==TerminalDisplayOperationKind::cp437_text) text_column_modulo_=(text_column_modulo_+addition)%8U;
        else if(kind==TerminalDisplayOperationKind::paging_newline || kind==TerminalDisplayOperationKind::logical_newline ||
            kind==TerminalDisplayOperationKind::clear_screen) text_column_modulo_=0;
        return true;
    }

    bool add_source_literal(
        const std::string_view literal,
        const bool ansi_source) {
        std::string text;
        try {
            text.reserve(literal.size() +
                std::min(literal.size(), kMaximumRenderedDisplayBytes));
        } catch (...) {
            return false;
        }
        const auto flush_text = [&]() {
            if (text.empty()) return true;
            const bool added = add_operation(
                TerminalDisplayOperationKind::cp437_text, std::move(text));
            text.clear();
            return added;
        };

        for (std::size_t index = 0; index < literal.size();) {
            const auto character =
                static_cast<unsigned char>(literal[index]);
            if (character == 0x1bU) {
                if (!ansi_source || !flush_text()) return false;
                const auto length =
                    safe_ansi_sequence_length(literal, index);
                if (!length.has_value()) return false;
                if (!add_operation(
                        TerminalDisplayOperationKind::ansi_sequence,
                        std::string(literal.substr(index, *length)))) {
                    return false;
                }
                index += *length;
                continue;
            }
            if (character == static_cast<unsigned char>('\r') ||
                character == static_cast<unsigned char>('\n')) {
                if (!flush_text()) return false;
                const bool count_for_paging =
                    character == static_cast<unsigned char>('\r') &&
                    index + 1U < literal.size() &&
                    literal[index + 1U] == '\n';
                if (count_for_paging) {
                    ++index;
                }
                if (!add_operation(count_for_paging
                        ? TerminalDisplayOperationKind::paging_newline
                        : TerminalDisplayOperationKind::logical_newline)) {
                    return false;
                }
                ++index;
                continue;
            }
            // Legacy ANSI art can contain a DLE filler byte. It has no SFX
            // presentation meaning: consume it without sending a terminal control.
            if (ansi_source && character == 0x10U) { ++index; continue; }
            if(character==static_cast<unsigned char>('\t')) {
                // Normalize ordinary text tabs into inert spaces. Track source
                // text columns across tokens/includes; ANSI styling is not text.
                const auto padding=8U-(text_column_modulo_+text.size())%8U;
                if(padding>kMaximumRenderedDisplayBytes-text.size()) return false;
                text.append(padding,' ');++index;continue;
            }
            if (character < 0x20U) return false;
            text.push_back(literal[index]);
            ++index;
        }
        return flush_text();
    }

    bool add_control(const std::string_view name) {
        if (name == "PROMPTOFF") {
            return add_operation(
                TerminalDisplayOperationKind::paging_disabled);
        }
        if (name == "PROMPT") {
            return add_operation(
                TerminalDisplayOperationKind::paging_enabled);
        }
        if (name == "NOABORT") {
            return add_operation(
                TerminalDisplayOperationKind::abort_disabled);
        }
        if (name == "ABORTON") {
            return add_operation(
                TerminalDisplayOperationKind::abort_enabled);
        }
        if (name == "BEEP") {
            return add_operation(TerminalDisplayOperationKind::caller_bell);
        }
        if (name == "CLS") {
            return add_operation(TerminalDisplayOperationKind::clear_screen);
        }
        if (name == "PAUSE") {
            return add_operation(TerminalDisplayOperationKind::pause);
        }
        return false;
    }

    bool add_value(const ParsedDisplayToken& token) {
        if (!eligible_value_token(request_.identity, token.name)) {
            return false;
        }
        std::string value;
        if (!context_value(request_, token.name, value)) return false;
        auto format = token.format;
        if (!trusted_menu_basename(request_.identity).empty() || request_.identity==TrustedDisplayIdentity::door_bulletin_page) {
            if (!format_menu_value(token, value)) return false;
            format.named_format.clear();
        }
        if (!apply_field_format(format, value) || !safe_context_value(value)) {
            return false;
        }
        return value.empty() || add_operation(
            TerminalDisplayOperationKind::cp437_text, std::move(value));
    }

    bool include_allowed(const std::string_view filename) const {
        const auto dot = filename.rfind('.');
        if (dot == std::string_view::npos) return false;
        const auto extension = upper(filename.substr(dot));
        if (request_.graphics == GraphicsMode::ascii) {
            return extension == ".ASC";
        }
        return extension == ".ASC" || extension == ".ANS";
    }

    TrustedDisplayAssetReadResult safe_read(
        const std::string_view logical_identity) {
        try {
            return reader_.read_asset(
                logical_identity, kMaximumTrustedDisplaySourceBytes);
        } catch (...) {
            return {TrustedDisplayAssetReadCode::unavailable, {}, {}, {}};
        }
    }

    bool render_asset(
        const TrustedDisplayAssetReadResult& asset,
        const std::size_t depth) {
        if (asset.code != TrustedDisplayAssetReadCode::available ||
            !valid_include_filename(asset.logical_identity) ||
            asset.stable_file_identity.empty() || asset.bytes.empty() ||
            asset.bytes.size() > kMaximumTrustedDisplaySourceBytes ||
            depth > kMaximumDisplayIncludeDepth) {
            return false;
        }
        if (asset.bytes.size() >
            kMaximumTrustedDisplayAggregateSourceBytes -
                aggregate_source_bytes_) {
            return false;
        }
        const auto logical = upper(asset.logical_identity);
        if (std::find(logical_stack_.begin(), logical_stack_.end(), logical) !=
                logical_stack_.end() ||
            std::find(file_stack_.begin(), file_stack_.end(),
                asset.stable_file_identity) != file_stack_.end()) {
            return false;
        }
        const auto dot = logical.rfind('.');
        if (dot == std::string::npos) return false;
        const auto extension = logical.substr(dot);
        const bool ansi_source = extension == ".ANS";
        if (extension != ".ASC" && extension != ".ANS") return false;
        if (request_.graphics == GraphicsMode::ascii && ansi_source) {
            return false;
        }

        aggregate_source_bytes_ += asset.bytes.size();
        logical_stack_.push_back(logical);
        file_stack_.push_back(asset.stable_file_identity);
        bool valid = true;
        const auto content = menu_asset_content(asset.bytes);
        if (!content.has_value()) {
            file_stack_.pop_back(); logical_stack_.pop_back(); return false;
        }
        const auto enclosing_bps = playback_bps_;
        playback_bps_ = ansi_source ? asset.playback_bps : 0;
        const DisplayParseResult parsed = parse_display(
            *content, request_.identity==TrustedDisplayIdentity::door_bulletin_generated?DisplayTrust::untrusted:DisplayTrust::trusted_sysop_asset, registry_);
        if (!parsed.valid()) valid = false;
        for (const auto& segment : parsed.segments) {
            if (!valid) break;
            if (segment.kind == DisplaySegmentKind::literal) {
                valid = add_source_literal(segment.literal, ansi_source);
                continue;
            }
            if (!segment.token.has_value()) {
                valid = false;
                break;
            }
            const auto& token = *segment.token;
            switch (token.kind) {
                case DisplayTokenKind::value:
                    valid = add_value(token);
                    break;
                case DisplayTokenKind::control:
                    valid = add_control(token.name);
                    break;
                case DisplayTokenKind::include: {
                    if (depth >= kMaximumDisplayIncludeDepth ||
                        include_count_ >=
                            kMaximumTrustedDisplayIncludeInstances ||
                        !include_allowed(token.include_filename)) {
                        valid = false;
                        break;
                    }
                    const auto included = safe_read(token.include_filename);
                    if (included.code !=
                        TrustedDisplayAssetReadCode::available) {
                        valid = false;
                        break;
                    }
                    ++include_count_;
                    valid = render_asset(included, depth + 1U);
                    break;
                }
            }
        }
        playback_bps_ = enclosing_bps;
        file_stack_.pop_back();
        logical_stack_.pop_back();
        return valid;
    }

    ITrustedDisplayAssetReader& reader_;
    const DisplayTokenRegistry& registry_;
    const TrustedDisplaySelectionRequest& request_;
    TerminalDisplayProgram output_;
    std::size_t aggregate_source_bytes_{0};
    std::size_t text_column_modulo_{0};
    std::size_t include_count_{0};
    std::uint32_t playback_bps_{};
    std::vector<std::string> logical_stack_;
    std::vector<std::string> file_stack_;
};

TrustedDisplaySelectionCode selection_code(
    const TrustedDisplayAssetReadCode code) noexcept {
    switch (code) {
        case TrustedDisplayAssetReadCode::available:
            return TrustedDisplaySelectionCode::available;
        case TrustedDisplayAssetReadCode::not_found:
            return TrustedDisplaySelectionCode::not_found;
        case TrustedDisplayAssetReadCode::invalid:
            return TrustedDisplaySelectionCode::invalid;
        case TrustedDisplayAssetReadCode::unavailable:
            return TrustedDisplaySelectionCode::unavailable;
    }
    return TrustedDisplaySelectionCode::unavailable;
}

std::string display_candidate(
    const TrustedDisplayIdentity identity,
    const GraphicsMode graphics) {
    const std::string extension =
        graphics == GraphicsMode::ansi ? ".ANS" : ".ASC";
    const auto menu_base = trusted_menu_basename(identity);
    if (!menu_base.empty()) return std::string(menu_base) + extension;
    switch (identity) {
        case TrustedDisplayIdentity::sfprelog: return "SFPRELOG.ASC";
        case TrustedDisplayIdentity::welcome1: return "WELCOME1" + extension;
        case TrustedDisplayIdentity::newreg: return "NEWREG" + extension;
        case TrustedDisplayIdentity::newuser: return "NEWUSER" + extension;
        case TrustedDisplayIdentity::nonodes: return "NONODES" + extension;
        case TrustedDisplayIdentity::all: return "ALL" + extension;
        case TrustedDisplayIdentity::chatdone: return "CHATDONE" + extension;
        case TrustedDisplayIdentity::eventime: return "EVENTIME" + extension;
        case TrustedDisplayIdentity::goodbye: return "GOODBYE" + extension;
        case TrustedDisplayIdentity::lockout: return "LOCKOUT" + extension;
        case TrustedDisplayIdentity::sfasleep: return "SFASLEEP" + extension;
        case TrustedDisplayIdentity::sfnewsletter: return "SFNWSLTR" + extension;
        default: break;
    }
    return {};
}

}  // namespace

bool TrustedDisplaySelectionResult::available() const noexcept {
    return code == TrustedDisplaySelectionCode::available &&
        program.has_value() &&
        valid_rendered_trusted_display_program(*program);
}

bool prepare_acknowledged_display(TerminalDisplayProgram& program, GraphicsMode graphics,
    std::optional<std::uint8_t> caller_color, bool blank_line_before_prompt) {
    using Op=TerminalDisplayOperationKind;
    if(!valid_terminal_display_program(program)) return false;
    const bool ansi=graphics==GraphicsMode::ansi;
    if(caller_color && *caller_color>15) return false;
    const auto color=ansi && caller_color ? menu_text_sgr(*caller_color) : std::string{};
    const std::size_t clear_ops=ansi?1U:25U, clear_bytes=ansi?7U:50U;
    const std::string prompt="Press <ENTER> to continue.";
    // Source CRLF is a paging row, but padding after the last visible text
    // must not ask MORE at EOF. Preserve every byte/layout newline; only its
    // automatic-paging eligibility changes. Explicit authored pauses remain.
    auto last=program.operations.end();
    for(auto it=program.operations.begin();it!=program.operations.end();++it)
        if(it->kind==Op::cp437_text && it->bytes.find_first_not_of(" \t")!=std::string::npos) last=it;
    unsigned trailing=0;
    for(auto it=last==program.operations.end()?program.operations.begin():std::next(last);it!=program.operations.end();++it)
        if(it->kind==Op::paging_newline || it->kind==Op::logical_newline) ++trailing;
    const unsigned separator=blank_line_before_prompt?(trailing>=2?0U:2U-trailing):1U;
    const auto extra_ops=clear_ops*2U+4U+separator+(color.empty()?0U:2U),
        extra_bytes=clear_bytes*2U+2U+separator*2U+prompt.size()+color.size()*2U;
    if(program.operations.size()>kTerminalMaximumDisplayOperations-extra_ops ||
        program.output_byte_count>kTerminalMaximumDisplayOutputBytes-extra_bytes) return false;
    const TerminalDisplayOperation clear{ansi?Op::clear_screen:Op::logical_newline,{}};
    for(auto it=last==program.operations.end()?program.operations.begin():std::next(last);it!=program.operations.end();++it)
        if(it->kind==Op::paging_newline) it->kind=Op::logical_newline;
    program.operations.insert(program.operations.begin(),clear_ops,clear);
    if(!color.empty()) {
        program.operations.insert(program.operations.begin()+clear_ops,{Op::ansi_sequence,color});
        program.paging_prompt_color=caller_color;
    }
    program.operations.push_back({Op::abort_enabled,{}});
    program.operations.push_back({Op::paging_disabled,{}});
    for(unsigned n=0;n<separator;++n)program.operations.push_back({Op::logical_newline,{}});
    if(!color.empty()) program.operations.push_back({Op::ansi_sequence,color});
    program.operations.push_back({Op::cp437_text,prompt});
    program.operations.push_back({Op::pause,{}});
    program.operations.insert(program.operations.end(),clear_ops,clear);
    program.output_byte_count+=extra_bytes;
    return valid_terminal_display_program(program);
}

namespace {
std::string door_display_base(TrustedDisplayIdentity id,std::uint16_t number){
    if(id!=TrustedDisplayIdentity::door_submenu && id!=TrustedDisplayIdentity::door_prelaunch&&id!=TrustedDisplayIdentity::door_bulletin_page&&id!=TrustedDisplayIdentity::door_bulletin_generated)return std::string(trusted_menu_basename(id));
    if(number<1 || number>999)return {};
    return std::string(id==TrustedDisplayIdentity::door_submenu?"DR":id==TrustedDisplayIdentity::door_prelaunch?"DP":"DB")+std::to_string(1000U+number).substr(1);
}
}
bool valid_trusted_display_selection_request(
    const TrustedDisplaySelectionRequest& request) noexcept {
    const bool door=request.identity==TrustedDisplayIdentity::door_submenu||request.identity==TrustedDisplayIdentity::door_prelaunch||request.identity==TrustedDisplayIdentity::door_bulletin_page||request.identity==TrustedDisplayIdentity::door_bulletin_generated;
    if(door){if(request.door_asset_number<1||request.door_asset_number>999)return false;}
    else if(request.door_asset_number)return false;
    if(request.bulletin_number<1 || request.bulletin_number>99 ||
        (request.identity!=TrustedDisplayIdentity::bulletin_page && request.bulletin_number!=1)) return false;
    if(request.welcome_number<1 || request.welcome_number>99 ||
        (request.identity!=TrustedDisplayIdentity::welcome1 && request.welcome_number!=1)) return false;
    if (request.graphics != GraphicsMode::ascii && request.graphics != GraphicsMode::ansi)
        return false;
    if (door || !trusted_menu_basename(request.identity).empty()) {
        return request.authenticated_security_level.has_value() &&
            core::SecurityLevel::create(*request.authenticated_security_level).has_value();
    }
    switch (request.identity) {
        case TrustedDisplayIdentity::sfprelog:
            return request.graphics == GraphicsMode::ascii &&
                !request.authenticated_security_level.has_value();
        case TrustedDisplayIdentity::welcome1:
        case TrustedDisplayIdentity::newreg:
        case TrustedDisplayIdentity::nonodes:
        case TrustedDisplayIdentity::all: case TrustedDisplayIdentity::chatdone:
        case TrustedDisplayIdentity::eventime: case TrustedDisplayIdentity::goodbye:
        case TrustedDisplayIdentity::lockout: case TrustedDisplayIdentity::sfasleep:
        case TrustedDisplayIdentity::sfnewsletter:
        case TrustedDisplayIdentity::bulletin_page:
            return !request.authenticated_security_level.has_value();
        case TrustedDisplayIdentity::newuser:
            return !request.authenticated_security_level.has_value() ||
                core::SecurityLevel::create(
                    *request.authenticated_security_level).has_value();
        default: break;
    }
    return false;
}

bool valid_rendered_trusted_display_program(
    const RenderedTrustedDisplayProgram& program) noexcept {
    if (program.graphics != GraphicsMode::ascii && program.graphics != GraphicsMode::ansi) return false;
    if (!valid_include_filename(program.selected_logical_asset) ||
        program.aggregate_source_bytes == 0 ||
        program.aggregate_source_bytes >
            kMaximumTrustedDisplayAggregateSourceBytes ||
        program.resolved_include_count >
            kMaximumTrustedDisplayIncludeInstances ||
        !valid_terminal_display_program(program.terminal_program)) {
        return false;
    }

    const auto& selected = program.selected_logical_asset;
    const bool ascii_asset = selected.size() > 4U &&
        selected.compare(selected.size() - 4U, 4U, ".ASC") == 0;
    const bool ansi_asset = selected.size() > 4U &&
        selected.compare(selected.size() - 4U, 4U, ".ANS") == 0;
    if ((program.graphics == GraphicsMode::ascii && !ascii_asset) ||
        (program.graphics == GraphicsMode::ansi && !ansi_asset && !ascii_asset)) {
        return false;
    }
    if (ascii_asset &&
        std::any_of(program.terminal_program.operations.begin(),
            program.terminal_program.operations.end(),
            [](const TerminalDisplayOperation& operation) {
                return operation.kind ==
                    TerminalDisplayOperationKind::ansi_sequence;
            })) {
        return false;
    }

    const auto menu_base = door_display_base(program.identity,program.door_asset_number);
    if((program.identity==TrustedDisplayIdentity::door_submenu || program.identity==TrustedDisplayIdentity::door_prelaunch||program.identity==TrustedDisplayIdentity::door_bulletin_page||program.identity==TrustedDisplayIdentity::door_bulletin_generated)?menu_base.empty():program.door_asset_number!=0)return false;
    if (!menu_base.empty()) {
        const auto base = std::string_view(selected).substr(0, selected.size() - 4U);
        if (!program.exact_security_match) return base == menu_base;
        if (base.size() <= menu_base.size() || base.substr(0, menu_base.size()) != menu_base)
            return false;
        const auto suffix = base.substr(menu_base.size());
        std::uint64_t level = 0;
        return suffix.size() <= 3U && suffix.front() != '0' &&
            parse_unsigned_decimal(suffix, level) && level >= 1U && level <= 999U;
    }

    switch (program.identity) {
        case TrustedDisplayIdentity::all: case TrustedDisplayIdentity::chatdone:
        case TrustedDisplayIdentity::eventime: case TrustedDisplayIdentity::goodbye:
        case TrustedDisplayIdentity::lockout: case TrustedDisplayIdentity::sfasleep:
        case TrustedDisplayIdentity::sfnewsletter:
            return !program.exact_security_match && selected == display_candidate(program.identity,ascii_asset?GraphicsMode::ascii:GraphicsMode::ansi);
        case TrustedDisplayIdentity::sfprelog:
            return !program.exact_security_match &&
                program.graphics == GraphicsMode::ascii &&
                selected == "SFPRELOG.ASC";
        case TrustedDisplayIdentity::welcome1: case TrustedDisplayIdentity::bulletin_page: {
            const auto prefix=program.identity==TrustedDisplayIdentity::welcome1?std::string_view("WELCOME"):std::string_view("BULLET");
            if(program.exact_security_match || selected.rfind(prefix,0)!=0) return false;
            const auto digits=std::string_view(selected).substr(prefix.size(),selected.size()-prefix.size()-4);
            std::uint64_t number=0;
            return !digits.empty() && digits.size()<=2 && digits.front()!='0' &&
                parse_unsigned_decimal(digits,number) && number>=1 && number<=99;
        }
        case TrustedDisplayIdentity::newreg:
            return !program.exact_security_match &&
                selected == (ansi_asset ? "NEWREG.ANS" : "NEWREG.ASC");
        case TrustedDisplayIdentity::nonodes:
            return !program.exact_security_match &&
                selected == (ansi_asset ? "NONODES.ANS" : "NONODES.ASC");
        case TrustedDisplayIdentity::newuser:
            if (!program.exact_security_match) {
                return selected ==
                    (ansi_asset ? "NEWUSER.ANS" : "NEWUSER.ASC");
            }
            if (selected.rfind("NEWUSER", 0U) != 0U) return false;
            {
                const auto digits = std::string_view(selected).substr(
                    7U, selected.size() - 7U - 4U);
                if (digits.empty() || digits.size() > 3U) return false;
                std::uint16_t level = 0U;
                for (const char character : digits) {
                    if (character < '0' || character > '9') return false;
                    level = static_cast<std::uint16_t>(
                        level * 10U +
                        static_cast<std::uint16_t>(character - '0'));
                }
                return core::SecurityLevel::create(level).has_value();
            }
        default: break;
    }
    return false;
}

TrustedDisplaySelector::TrustedDisplaySelector(
    ITrustedDisplayAssetReader& reader) noexcept
    : reader_(reader) {}

std::optional<std::string_view> display_asset_content(std::string_view bytes){return menu_asset_content(bytes);}

TrustedDisplaySelectionResult TrustedDisplaySelector::select(
    const TrustedDisplaySelectionRequest& request) {
    if (!valid_trusted_display_selection_request(request)) {
        return {TrustedDisplaySelectionCode::invalid, std::nullopt};
    }
    try {
        std::vector<std::pair<std::string, bool>> candidates;
        const auto menu_base = door_display_base(request.identity,request.door_asset_number);
        if(request.identity==TrustedDisplayIdentity::menu_bulletins) {
            // Regular bulletins are file-driven and available to all callers.
            // BULLETIN10 is not an access-specific regular bulletin menu.
            candidates.push_back({std::string(menu_base)+(request.graphics==GraphicsMode::ansi?".ANS":".ASC"),false});
        } else if ((request.identity == TrustedDisplayIdentity::newuser && request.authenticated_security_level) || !menu_base.empty()) {
            const auto level = core::SecurityLevel::create(
                *request.authenticated_security_level);
            if (!level.has_value()) {
                return {TrustedDisplaySelectionCode::invalid, std::nullopt};
            }
            const auto plan = make_display_lookup_plan(
                menu_base.empty() ? "NEWUSER" : menu_base, request.graphics, *level);
            if (!plan.has_value()) {
                return {TrustedDisplaySelectionCode::invalid, std::nullopt};
            }
            candidates.push_back({plan->exact_security_candidate, true});
            candidates.push_back({plan->fallback_candidate, false});
        } else {
            candidates.push_back(
                {request.identity==TrustedDisplayIdentity::welcome1
                    ? "WELCOME"+std::to_string(request.welcome_number)+(request.graphics==GraphicsMode::ansi?".ANS":".ASC")
                    : request.identity==TrustedDisplayIdentity::bulletin_page
                    ? "BULLET"+std::to_string(request.bulletin_number)+(request.graphics==GraphicsMode::ansi?".ANS":".ASC")
                    : display_candidate(request.identity, request.graphics), false});
        }

        // Graphics selection is shared by menus, welcomes and optional event
        // files: prefer all matching ANSI candidates, then matching plain files.
        // ASCII callers never gain an ANSI candidate or ANSI source controls.
        if(request.graphics==GraphicsMode::ansi) {
            const auto ansi_candidates=candidates;
            for(const auto& candidate:ansi_candidates) {
                auto plain=candidate.first;
                if(plain.size()>4 && plain.substr(plain.size()-4)==".ANS") {
                    plain.replace(plain.size()-4,4,".ASC");
                    candidates.push_back({std::move(plain),candidate.second});
                }
            }
        }

        TrustedDisplaySelectionCode last =
            TrustedDisplaySelectionCode::not_found;
        for (const auto& candidate : candidates) {
            TrustedDisplayAssetReadResult read;
            try {
                read = reader_.read_asset(
                    candidate.first, kMaximumTrustedDisplaySourceBytes);
            } catch (...) {
                read.code = TrustedDisplayAssetReadCode::unavailable;
            }
            // A missing later fallback must not disguise an invalid or
            // unreadable preferred file when no candidate can be rendered.
            if(read.code!=TrustedDisplayAssetReadCode::not_found ||
                last==TrustedDisplaySelectionCode::not_found)
                last = selection_code(read.code);
            if (read.code != TrustedDisplayAssetReadCode::available) {
                continue;
            }
            if (!ascii_case_equal(read.logical_identity, candidate.first)) {
                last = TrustedDisplaySelectionCode::invalid;
                continue;
            }
            TrustedDisplayRenderer renderer(reader_, registry_, request);
            auto program = renderer.render(read, candidate.second);
            if (!program.has_value()) {
                last = TrustedDisplaySelectionCode::invalid;
                continue;
            }
            return {TrustedDisplaySelectionCode::available,
                std::move(program)};
        }
        return {last, std::nullopt};
    } catch (...) {
        return {TrustedDisplaySelectionCode::unavailable, std::nullopt};
    }
}

}  // namespace sfx::application
