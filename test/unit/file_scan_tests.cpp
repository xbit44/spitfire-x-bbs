#include "sfx/application/file_scan.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app = sfx::application;
namespace {
int checks{};
void check(bool value, const char* what) { ++checks; if (!value) throw std::runtime_error(what); }
const std::string path = "C:\\SFX\\Data\\FileIntake\\0123456789abcdef\\payload.bin";
std::string summary(unsigned infected = 0) {
    return "\r\n----------- SCAN SUMMARY -----------\r\nKnown viruses: 3628169\r\nEngine version: 1.4.7\r\nScanned directories: 0\r\nScanned files: 1\r\nInfected files: " +
        std::to_string(infected) + "\r\nData scanned: 0.00 MB\r\nData read: 0.00 MB (ratio 0.00:1)\r\nTime: 10.446 sec (0 m 10 s)\r\nStart Date: 2026:10:17 08:06:43\r\nEnd Date:   2026:10:17 08:06:53\r\n";
}
app::FileScanResult parse(std::string_view out, unsigned exit = 0, std::string_view error = {}) {
    return app::classify_clamav_scan({true, true, false, false, true, exit, path, out, error});
}
}
int main() { try {
    const auto clean = path + ": OK\r\n" + summary();
    check(parse(clean).verdict == app::FileScanVerdict::clean, "actual pinned clean output grammar");
    check(app::file_scan_report(parse(clean), false, false) == "Scan passed - not added", "pass is not acceptance");
    const auto detected = path + ": Synthetic.Test.Signature FOUND\r\n" + summary(1);
    check(parse(detected, 1).verdict == app::FileScanVerdict::detected, "synthetic positive parse only");
    check(parse(detected, 1).detections == std::vector<std::string>{"Synthetic.Test.Signature"}, "only actual returned name");
    const auto multi = path + ": Synthetic.First FOUND\n" + path + ": Synthetic.Second FOUND\n" + summary(1);
    check(parse(multi, 1).detections.size() == 2, "allmatch bounded multiple results");
    for (const auto* name : {"Heuristics.Encrypted.Zip", "Heuristics.Limits.Exceeded.MaxFileSize", "Heuristics.Broken.Executable", "Heuristics.Phishing.Test", "Custom.UNOFFICIAL"}) {
        const auto result = parse(path + ": " + name + " FOUND\n" + summary(1), 1);
        check(result.verdict == app::FileScanVerdict::incomplete && app::incoming_file_action(true, result) == app::IncomingFileAction::durable_private_hold, "incomplete is not malware deletion");
    }
    for (const auto& text : {std::string{}, path + ": OK\n", summary(), clean + "WARNING: skipped\n", "WARNING: stale\n" + clean,
        "wrong-path: OK\n" + summary(), path + ": Empty file\n" + summary(), path + ": Access denied ERROR\n" + summary(),
        path + ": OK\n" + path + ": OK\n" + summary(), path + ": OK\n" + detected, detected + summary(1),
        path + ": Bad\x1b[31mName FOUND\n" + summary(1), path + ": ../path FOUND\n" + summary(1)}) {
        check(parse(text).verdict != app::FileScanVerdict::clean && parse(text, 1).verdict != app::FileScanVerdict::detected, "unexpected output fails closed");
    }
    for (std::size_t i = 0; i < clean.size(); ++i) check(parse(clean.substr(0, i)).verdict != app::FileScanVerdict::clean, "every truncated report held");
    for (auto pair : {std::pair{"Known viruses: 3628169", "Known viruses: 0"}, {"Engine version: 1.4.7", "Engine version: 1.5.4"},
        {"Scanned directories: 0", "Scanned directories: 1"}, {"Scanned files: 1", "Scanned files: 0"},
        {"Scanned files: 1", "Scanned files: 2"}, {"Infected files: 0", "Infected files: 1"}, {"Known viruses: 3628169", "Known viruses: 9999999999999999999999999999"}}) {
        auto changed = clean; changed.replace(changed.find(pair.first), std::string_view(pair.first).size(), pair.second);
        check(parse(changed).verdict != app::FileScanVerdict::clean, "summary/version mismatch held");
    }
    check(parse(clean, 1).verdict != app::FileScanVerdict::clean && parse(detected, 0).verdict != app::FileScanVerdict::detected, "exit evidence agreement");
    check(parse(clean, 2).verdict == app::FileScanVerdict::error && parse(clean, 0, "warning").verdict == app::FileScanVerdict::error, "errors never clean");
    app::FileScanEvidence e{true, true, false, false, true, 0, path, clean, {}};
    e.started = false; check(app::classify_clamav_scan(e).verdict == app::FileScanVerdict::error, "launch failure"); e.started = true;
    e.exited = false; check(app::classify_clamav_scan(e).verdict == app::FileScanVerdict::error, "missing process completion"); e.exited = true;
    e.output_complete = false; check(app::classify_clamav_scan(e).verdict == app::FileScanVerdict::incomplete, "capture overflow or incomplete EOF");
    e.timed_out = true; check(app::classify_clamav_scan(e).verdict == app::FileScanVerdict::timeout, "actual deadline"); e.timed_out = false;
    e.cancelled = true; check(app::classify_clamav_scan(e).verdict == app::FileScanVerdict::incomplete, "cancelled");
    check(parse(clean + std::string(app::kFileScanOutputBytes, ' ')).verdict == app::FileScanVerdict::incomplete, "bounded output");
    std::string many;
    for (unsigned n = 0; n <= app::kFileScanDetectionMaximum; ++n) many += path + ": Test." + std::to_string(n) + " FOUND\n";
    check(parse(many + summary(1), 1).verdict == app::FileScanVerdict::incomplete, "bounded detection list");
    for (unsigned t = 0; t < 30000; t += 100) {
        const auto line = app::file_scan_activity(t);
        check(line.size() == 24 && std::count(line.begin(), line.end(), '#') == 5 && line.find_first_not_of(" #") == std::string::npos, "bounded slow hash activity not progress");
        if (t % 500) check(line == app::file_scan_activity(t - t % 500), "activity changes no faster than 500ms");
    }
    std::cout << "PASS file scan parser/activity " << checks << " checks (synthetic outcomes, not real engine scans)\n"; return 0;
} catch (const std::exception& e) { std::cerr << "FAIL file scan after " << checks << " checks: " << e.what() << '\n'; return 1; } }
