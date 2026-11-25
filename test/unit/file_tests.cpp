#include "sfx/application/file.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app = sfx::application;
namespace {
unsigned checks{};
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
#include "../../src/application/src/file_unicode_fold.inc"
std::string scalar_utf8(std::uint32_t c){
    if(c<128)return std::string(1,static_cast<char>(c));
    if(c<2048)return {static_cast<char>(192|(c>>6)),static_cast<char>(128|(c&63))};
    if(c<65536)return {static_cast<char>(224|(c>>12)),static_cast<char>(128|((c>>6)&63)),static_cast<char>(128|(c&63))};
    return {static_cast<char>(240|(c>>18)),static_cast<char>(128|((c>>12)&63)),static_cast<char>(128|((c>>6)&63)),static_cast<char>(128|(c&63))};
}
}
int main() { try {
    app::FilePolicy policy;
    check(app::valid_file_policy(policy) && policy.maximum_upload_bytes == 104857600 &&
        !policy.counts.daily_downloads && !policy.counts.ratio, "fresh size/count defaults; no invented ratio");
    app::FileGroup group{{1}, "Main", 0, 1, 1, true};
    app::FileArea area;
    area.id = {1}; area.group = {1}; area.number = 1; area.name = "General Files";
    check(app::valid_file_group(group) && app::valid_file_area(area), "default group/area valid");
    check(!area.permissions.uploads_enabled && !area.backup_contents && !area.own_description_edit, "ordinary-area uploads/backup/edit Off");
    const app::FileAuthority caller{10, 90}, sysop{90, 90};
    check(app::file_area_visible(group, area, caller) && app::file_area_downloadable(group, area, caller), "ordinary visible/downloadable");
    check(!app::file_area_uploadable(group, area, caller, false), "ordinary uploads require explicit enable");
    check(app::file_area_uploadable(group,area,sysop,false),"caller-upload switch does not prohibit an authorized Sysop");
    area.permissions.uploads_enabled = true;
    check(app::file_area_uploadable(group, area, caller, false), "enabled ordinary upload");
    area.kind = app::FileAreaKind::review_uploads; area.permissions.downloads_sysop_only = true;
    check(app::valid_file_area(area) && app::file_area_visible(group, area, caller) &&
        !app::file_area_downloadable(group, area, caller) && app::file_area_downloadable(group, area, sysop), "review queue visible but Sysop download only");
    area.kind = app::FileAreaKind::private_sysop;
    check(!app::file_area_visible(group, area, caller) && !app::file_area_downloadable(group, area, caller) &&
        app::file_area_uploadable(group, area, caller, true), "private direct submission never grants browse");
    check(!app::file_area_uploadable(group, area, caller, false) && app::file_area_visible(group, area, sysop), "private submission route explicit; configured Sysop threshold");
    area.permissions.access=999;area.permissions.upload=10;area.permissions.download=999;
    const app::FileAuthority owner{999,999}, ordinary{10,999};
    check(app::file_area_uploadable(group,area,ordinary,true)&&!app::file_area_visible(group,area,ordinary)&&!app::file_area_downloadable(group,area,ordinary),"private access999/upload10 accepts submissions without exposing catalog");
    check(!app::file_area_uploadable(group,area,ordinary,false)&&!app::file_area_uploadable(group,area,{9,999},true),"private normal route and below-upload-minimum callers denied");
    check(app::file_area_uploadable(group,area,owner,false)&&app::file_area_downloadable(group,area,owner),"Sysop can use normal upload in selected private area");
    group.access=20;check(!app::file_area_uploadable(group,area,ordinary,true),"private route still honors group access");group.access=1;
    area.enabled=false;check(!app::file_area_uploadable(group,area,owner,true),"Sysop cannot upload to disabled area");area.enabled=true;
    area.permissions={1,1,1,true,true};
    for (unsigned n = 1; n <= 999; ++n) {
        const app::FileAuthority a{static_cast<std::uint16_t>(n), 77};
        check(app::file_area_visible(group, area, a) == (n >= 77), "no hardcoded Sysop level");
    }
    check(!app::file_area_visible(group, area, {999, 0}) && !app::file_area_uploadable(group, area, {0, 90}, true), "invalid authority fails closed");
    group.enabled = false;
    check(!app::file_area_uploadable(group, area, sysop, true), "disabled group still denies private submission"); group.enabled = true;
    area.permissions.upload = 20;
    check(!app::file_area_uploadable(group, area, caller, true), "upload threshold independently checked"); area.permissions.upload = 1;
    check(app::file_upload_limit(policy, area) == 104857600, "area inherits global upload cap");
    area.maximum_upload_bytes = 4096; check(app::file_upload_limit(policy, area) == 4096, "area override");
    area.maximum_upload_bytes = 0; check(app::file_upload_limit(policy, area) == 0, "invalid cap fails closed"); area.maximum_upload_bytes.reset();
    for (const auto name : {"a.txt", "my file.zip", "demo.ANS", "a..b", "File-1_(x).zip"}) check(app::valid_file_name(name), "safe basename");
    for (const auto name : {"", ".", "..", "../x", "x/y", "x\\y", "C:x", "x:stream", "x.", "x ", " x", "CON.txt", "COM1.ZIP", "lpt0.zip", "NUL", "CONOUT$", "a*", "a?", "a\n", "a\x1b"}) check(!app::valid_file_name(name), "unsafe basename rejected");
    check(app::valid_file_name(std::string(128, 'x')) && !app::valid_file_name(std::string(129, 'x')), "filename boundary");
    for(const auto name:{"CON .txt","nul  .zip","LPT1 .txt","COM\xc2\xb9.txt","LPT\xc2\xb2.zip","COM\xc2\xb3"})
        check(!app::valid_file_name(name),"Windows device aliases refused before path use");
    const auto diz = app::file_diz_descriptions("\x1b[31mUseful first line\x1b[0m\r\nMore\ttext\r\n");
    check(diz && diz->short_text == "Useful first line" && diz->long_text == "Useful first line\nMore text", "DIZ safely normalized");
    check(app::sanitize_file_text("a\x1b]52;c;secret\x07" "b\x1bPdata\x1b\\c", 100, true) == "abc", "OSC/DCS payload stripped");
    check(app::sanitize_file_text("a\r\nb\rc\nd", 100, true) == "a\nb\nc\nd", "newline normalization");
    check(!app::file_diz_descriptions("\n\t\x1b[31m") && !app::file_diz_descriptions(std::string(16385, 'x')), "missing/oversized DIZ unusable");
    check(app::file_diz_descriptions("\\untrusted")->short_text == "\\untrusted", "DIZ is text, not backslash authority");
    check(app::file_import_descriptions("demo.zip", std::nullopt).short_text == "demo.zip", "import filename fallback");
    check(app::file_import_descriptions("demo.zip", "\x1b[31m").short_text == "demo.zip", "unusable DIZ import has no forced prompt");
    check(app::file_name_matches("File_Name.ZIP", "name") && app::file_name_matches("File_Name.ZIP", "f*?.zip") &&
        !app::file_name_matches("File_Name.ZIP", "*.txt") && !app::file_name_matches("a", ""), "filename partial/wildcard case folding");
    for(const auto& pair:file_unicode_folds){
        const auto result=app::file_case_key(scalar_utf8(pair.source));
        check(result&&*result==scalar_utf8(pair.target)&&app::file_case_key(*result)==result,"every pinned Unicode simple mapping and idempotence");
    }
    check(app::valid_file_name(u8"École-Σ-Я-\U00010400.zip")&&app::file_name_matches(u8"École-Σ-Я-\U00010400.zip",u8"éCOLE-ς-я-\U00010428.ZIP"),"accent Greek Cyrillic supplementary case equivalence");
    check(app::file_name_matches(u8"É.zip","?.ZIP")&&!app::file_name_matches(u8"É.zip","??.zip"),"wildcard question consumes one scalar not one UTF-8 byte");
    check(app::file_case_key(u8"ẞ")==app::file_case_key(u8"ß")&&app::file_case_key(u8"ß")!=app::file_case_key("SS"),"simple folding preserves one-character filename semantics");
    check(app::file_case_key(u8"é")!=app::file_case_key(u8"e\u0301"),"no silent filename normalization or display rewriting");
    for(const auto bad:{"\x80","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82"})check(!app::valid_file_name(bad)&&!app::file_case_key(bad),"malformed UTF-8 refused before catalog/native IO");
    check(!app::valid_file_name(u8"report\u202eexe.txt")&&app::sanitize_file_text(u8"report\u202e.txt",80,false)=="report.txt","bidi control not a filename or display instruction");
    check(app::sanitize_file_text(u8"aé",2,false)=="a"&&app::sanitize_file_text(u8"aé",3,false)==u8"aé","byte-bound metadata never cuts a UTF-8 scalar");
    check(app::file_diz_descriptions("\x90" "cole")->short_text==u8"École"&&app::file_description_matches({u8"École",u8"Σample"},{u8"éCOLE"}),"legacy CP437 DIZ decoded before Unicode metadata matching");
    const auto terms = app::file_description_terms(" one ;TWO; three ");
    check(terms && terms->size() == 3 && app::file_description_matches({"something", "The TWO line"}, *terms), "description OR terms");
    for (const auto bad : {"", "a;", ";a", "a;;b", "a;b;c;d;e;f;g", "a\n"}) check(!app::file_description_terms(bad), "invalid description query");
    app::FileCountRule rule; app::FileCountState state;
    state.successful_downloads = UINT64_MAX; state.today_downloads = UINT64_MAX; state.eligible_downloads = UINT64_MAX;
    check(app::file_download_allowance(rule, state, true, false, false) == app::FileDownloadAllowance::allowed, "Off policies really unlimited");
    state = {}; rule.ratio = app::FileRatio{10, 1};
    check(app::file_ratio_entitlement(*rule.ratio, 0) == 10 && app::file_ratio_entitlement(*rule.ratio, 1) == 20 &&
        app::file_ratio_entitlement(*rule.ratio, 2) == 30, "owner exact one-time starter formula");
    state.eligible_downloads = 9;
    check(app::file_download_allowance(rule, state, true, false, false) == app::FileDownloadAllowance::allowed, "last starter slot");
    state.reserved_eligible_downloads = 1;
    check(app::file_download_allowance(rule, state, true, false, false) == app::FileDownloadAllowance::ratio_limit, "concurrent reservations consume allowance");
    check(app::file_download_allowance(rule, state, false, false, false) == app::FileDownloadAllowance::allowed, "area ratio exemption");
    check(app::file_download_allowance(rule, state, true, false, true) == app::FileDownloadAllowance::allowed, "caller ratio exemption");
    rule.daily_downloads = 1; state.today_downloads = 1;
    check(app::file_download_allowance(rule, state, false, false, true) == app::FileDownloadAllowance::daily_limit, "daily limit independent of ratio");
    state.today_downloads = 0; state.reserved_today_downloads = 1;
    check(app::file_download_allowance(rule, state, false, false, true) == app::FileDownloadAllowance::daily_limit, "daily concurrent reservation");
    check(app::file_download_allowance(rule, state, false, true, true) == app::FileDownloadAllowance::allowed, "independent exemption");
    rule.ratio = app::FileRatio{};
    check(app::file_download_allowance(rule, state, true, true, true) == app::FileDownloadAllowance::invalid_policy, "no empty default ratio");
    check(app::file_ratio_entitlement({1000000, 1}, UINT64_MAX) == UINT64_MAX &&
        app::file_ratio_entitlement({2, 3}, 5) == 5, "ratio overflow/fraction floor safe");
    app::FileUploadQueue uploads;
    check(uploads.add({"x.zip", {1}, {"description", ""}, false}) && !uploads.add({"X.ZIP", {2}, {"description", ""}, false}), "batch names unambiguous across destinations");
    app::FileUploadQueue private_uploads;
    check(private_uploads.add({"private.zip", {3}, {"private", ""}, false}) && uploads.items().size() == 1 && uploads.items()[0].destination.value == 1, "private queue is separate");
    private_uploads.clear(); check(uploads.items().size() == 1 && !uploads.remove(1) && uploads.remove(0), "queue clear/remove are metadata-only");
    app::FileDownloadQueue downloads;
    for (std::size_t n = 1; n <= app::kFileQueueMaximum; ++n) check(downloads.add({static_cast<std::int64_t>(n)}), "bounded download queue");
    check(!downloads.add({1}) && !downloads.add({257}) && downloads.remove({1}) && downloads.add({257}), "duplicate/cap/remove");
    app::FileEntry entry;
    entry.id = {1}; entry.area = area.id; entry.uploader = 42;
    check(!app::file_entry_visible(group, area, entry, caller), "private metadata never visible");
    area.kind = app::FileAreaKind::ordinary; area.permissions.downloads_sysop_only = false;
    check(!app::file_description_editable(group, area, entry, caller, 42), "own-description editing defaults Off");
    area.own_description_edit = true;
    check(app::file_description_editable(group, area, entry, caller, 42) && !app::file_description_editable(group, area, entry, caller, 43), "own text only");
    entry.origin = app::FileOrigin::sysop_import;
    check(!app::file_description_editable(group, area, entry, caller, 42), "import attribution does not grant caller ownership");
    entry.origin = app::FileOrigin::caller_upload; entry.sysop_only = true;
    check(!app::file_entry_visible(group, area, entry, caller), "individual Sysop-only visibility");
    entry.sysop_only = false; entry.state = app::FileCatalogState::missing;
    check(!app::file_entry_visible(group, area, entry, sysop), "missing payload excluded from caller lists including Sysop");
    app::FileScanResult scan;
    check(app::incoming_file_action(true, scan) == app::IncomingFileAction::durable_private_hold, "default error holds");
    scan.verdict = app::FileScanVerdict::clean;
    check(app::incoming_file_action(true, scan) == app::IncomingFileAction::validate_and_commit && app::file_scan_report(scan, false, false) == "Scan passed - not added", "scan clean not a commit claim");
    scan.verdict = app::FileScanVerdict::detected;
    check(app::incoming_file_action(true, scan) == app::IncomingFileAction::durable_private_hold, "no detection evidence no deletion");
    scan.detections = {"Test.Signature\x1b[31m"};
    check(app::incoming_file_action(true, scan) == app::IncomingFileAction::delete_detected_staging, "positive detection exact incoming only");
    check(app::file_scan_report(scan, false, false).find("held; deletion not confirmed") != std::string::npos &&
        app::file_scan_report(scan, false, true) == "Detected - deleted: Test.Signature", "deletion report requires actual success; sanitized detection");
    for (const auto verdict : {app::FileScanVerdict::error, app::FileScanVerdict::timeout, app::FileScanVerdict::incomplete, app::FileScanVerdict::not_scanned}) {
        scan.verdict = verdict;
        check(app::incoming_file_action(true, scan) == app::IncomingFileAction::durable_private_hold, "incomplete/error/timeout/skipped never clean");
    }
    scan.verdict = app::FileScanVerdict::not_scanned;
    check(app::incoming_file_action(false, scan) == app::IncomingFileAction::validate_and_commit && app::file_scan_report(scan, true, false) == "Added - scanning Off", "scan Off honest success");
    std::cout << "PASS file model " << checks << " checks\n"; return 0;
} catch (const std::exception& e) { std::cerr << "FAIL file model after " << checks << " checks: " << e.what() << '\n'; return 1; } }
