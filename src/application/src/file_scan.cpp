#include "sfx/application/file_scan.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::application {
namespace {
bool starts(std::string_view value, std::string_view prefix) { return value.substr(0, prefix.size()) == prefix; }
bool ends(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}
bool number(std::string_view text, std::uint64_t& n) {
    if (text.empty() || text.size() > 20) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool printable(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
bool signature(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    });
}
FileScanResult hold(FileScanVerdict verdict, const char* detail) { return {verdict, {}, detail}; }
} // namespace

FileScanResult classify_clamav_scan(const FileScanEvidence& e) {
    if (e.timed_out) return hold(FileScanVerdict::timeout, "Scanner deadline exceeded");
    if (e.cancelled) return hold(FileScanVerdict::incomplete, "Scan cancelled before acceptance");
    if (!e.started || !e.exited) return hold(FileScanVerdict::error, "Scanner did not finish");
    if (!e.output_complete || e.standard_output.size() > kFileScanOutputBytes ||
        e.standard_error.size() > kFileScanOutputBytes || e.standard_output.size() + e.standard_error.size() > kFileScanOutputBytes)
        return hold(FileScanVerdict::incomplete, "Scanner report incomplete");
    if (e.exit_code > 1 || !e.standard_error.empty()) return hold(FileScanVerdict::error, "Scanner reported an error");
    if (e.expected_output_path.empty() || e.expected_output_path.size() > 4096 || !printable(e.expected_output_path) ||
        e.standard_output.empty() || e.standard_output.back() != '\n') return hold(FileScanVerdict::incomplete, "Scanner report not verifiable");

    const std::string prefix = std::string(e.expected_output_path) + ": ";
    const std::string_view headings[]{"Known viruses: ", "Engine version: ", "Scanned directories: ",
        "Scanned files: ", "Infected files: ", "Data scanned: ", "Data read: ", "Time: ", "Start Date: ", "End Date:   "};
    bool summary{}, ok{}, incomplete{};
    std::size_t field{};
    std::uint64_t infected{};
    std::vector<std::string> detections;
    std::string_view remaining = e.standard_output;
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        if (end == std::string_view::npos) return hold(FileScanVerdict::incomplete, "Scanner report truncated");
        auto line = remaining.substr(0, end); remaining.remove_prefix(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.size() > 8192 || !printable(line)) return hold(FileScanVerdict::incomplete, "Scanner report invalid");
        if (!summary && starts(line, prefix)) {
            auto status = line.substr(prefix.size());
            if (status == "OK") {
                if (ok || !detections.empty() || incomplete) return hold(FileScanVerdict::incomplete, "Conflicting scanner results");
                ok = true;
            } else if (ends(status, " FOUND")) {
                status.remove_suffix(6);
                if (ok || !signature(status) || detections.size() >= kFileScanDetectionMaximum)
                    return hold(FileScanVerdict::incomplete, "Scanner detection report invalid");
                // These alerts describe inability to inspect fully, not positive
                // malware evidence. Conservatively hold other heuristic results
                // too; unknown/unofficial signatures never justify auto-delete.
                if (starts(status, "Heuristics.") || ends(status, ".UNOFFICIAL")) incomplete = true;
                if (std::find(detections.begin(), detections.end(), status) == detections.end()) detections.emplace_back(status);
            } else return hold(FileScanVerdict::incomplete, "File was not fully scanned");
            continue;
        }
        if (line == "----------- SCAN SUMMARY -----------" && !summary) { summary = true; continue; }
        if (!summary || field >= std::size(headings) || !starts(line, headings[field]))
            return hold(FileScanVerdict::incomplete, "Unexpected scanner report");
        const auto value = line.substr(headings[field].size());
        std::uint64_t n{};
        switch (field) {
        case 0: if (!number(value, n) || n == 0) return hold(FileScanVerdict::error, "No verified scan definitions"); break;
        case 1: if (value != kClamAvEngineVersion) return hold(FileScanVerdict::error, "Unsupported scanner version"); break;
        case 2: if (!number(value, n) || n != 0) return hold(FileScanVerdict::incomplete, "Unexpected recursive scan"); break;
        case 3: if (!number(value, n) || n != 1) return hold(FileScanVerdict::incomplete, "Expected file was not fully scanned"); break;
        case 4: if (!number(value, infected) || infected > 1) return hold(FileScanVerdict::incomplete, "Conflicting scanner summary"); break;
        default: if (value.empty() || value.size() > 128) return hold(FileScanVerdict::incomplete, "Scanner summary invalid"); break;
        }
        ++field;
    }
    if (!summary || field != std::size(headings) || incomplete) return hold(FileScanVerdict::incomplete, "Scan incomplete or heuristic-only result");
    if (e.exit_code == 0 && ok && detections.empty() && infected == 0) return {FileScanVerdict::clean, {}, "One file scanned"};
    if (e.exit_code == 1 && !ok && !detections.empty() && infected == 1) return {FileScanVerdict::detected, std::move(detections), "Positive detection"};
    return hold(FileScanVerdict::incomplete, "Scanner exit and result disagree");
}
std::string file_scan_activity(std::uint64_t elapsed) {
    constexpr std::size_t width = 24, hashes = 5, steps = width - hashes;
    const auto phase = static_cast<std::size_t>((elapsed / 500) % (steps * 2));
    const auto at = phase <= steps ? phase : steps * 2 - phase;
    std::string line(width, ' '); line.replace(at, hashes, hashes, '#'); return line;
}
} // namespace sfx::application
