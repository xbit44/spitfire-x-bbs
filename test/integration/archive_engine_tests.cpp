#include "sfx/archive/archive_engine.hpp"
#include "sfx/archive/backup_service.hpp"
#include "sfx/archive/managed_journal_snapshot.hpp"
#include "../../src/apps/product_version.h"

#include <archive.h>
#include <archive_entry.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace archive_engine = sfx::archive;

namespace {

unsigned checks{};
void require(const bool condition, const char* const message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "create test file");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "write test file");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "open test file");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

int open_writer(struct archive* writer, const std::filesystem::path& path) {
#ifdef _WIN32
    return archive_write_open_filename_w(writer, path.c_str());
#else
    return archive_write_open_filename(writer, path.c_str());
#endif
}

void write_raw_entry(struct archive* writer, const std::string& path,
                     const std::string& content, const int type = AE_IFREG,const unsigned short mode=0600) {
    struct archive_entry* entry = archive_entry_new();
    require(entry != nullptr, "allocate raw ZIP entry");
    archive_entry_set_pathname_utf8(entry, path.c_str());
    archive_entry_set_filetype(entry, type);
    archive_entry_set_perm(entry, mode);
    archive_entry_set_size(entry, type == AE_IFREG
                                      ? static_cast<la_int64_t>(content.size())
                                      : 0);
    require(archive_write_header(writer, entry) == ARCHIVE_OK,
            "write raw ZIP header");
    if (type == AE_IFREG) {
        require(archive_write_data(writer, content.data(), content.size()) ==
                    static_cast<la_ssize_t>(content.size()),
                "write raw ZIP content");
    }
    archive_entry_free(entry);
}

void create_raw_zip(const std::filesystem::path& path,
                    const std::string& entry_path,
                    const std::string& content) {
    struct archive* writer = archive_write_new();
    require(writer != nullptr, "allocate raw ZIP writer");
    require(archive_write_set_format_zip(writer) == ARCHIVE_OK,
            "select raw ZIP format");
    require(open_writer(writer, path) == ARCHIVE_OK, "open raw ZIP");
    write_raw_entry(writer, entry_path, content);
    require(archive_write_close(writer) == ARCHIVE_OK, "close raw ZIP");
    require(archive_write_free(writer) == ARCHIVE_OK, "free raw ZIP");
}

archive_engine::Metadata metadata() {
    archive_engine::Metadata value;
    value.backup_type = "integration-test";
    value.transaction_id = "0123456789ABCDEF";
    value.sfx_version = "0.0.2-alpha";
    value.source_generation = "TEST-GENERATION";
    value.created = archive_engine::current_timestamp();
    return value;
}

archive_engine::Timestamp timestamp(const std::string& component) {
    return archive_engine::Timestamp{component, "2026-09-14T08:30:00", "-07:00",
                                     "2026-09-14T15:30:00Z"};
}

void test_backup_round_trip(const std::filesystem::path& root) {
    const auto source = root / "source";
    const auto backup = root / "Backup";
    std::filesystem::create_directories(backup);
    write_text(source / "Config" / "SFX.INI", "alpha=1\n");
    write_text(source / "Data" / "SFX.DB", std::string(131072, 'D'));
    write_text(source / "Files" / "welcome.txt", "welcome\n");
    write_text(source / "Doors" / "door.cfg", "door\n");
    write_text(source / "Logs" / "CALLERS.LOG", "caller\n");
    write_text(source / "Displays" / "welcome.ans", "display\n");
    write_text(source / "Unknown" / "owned.bin", "unknown\n");
    write_text(source / "Backups" / "legacy.txt", "legacy plural backup\n");
    write_text(source / "Backup" / "recursive.zip", "excluded\n");
    write_text(source / "Work" / "scratch.tmp", "excluded\n");
    write_text(source / "Bin" / "SFXRuntime.exe", "excluded\n");

    const auto final_path = backup / archive_engine::backup_filename(metadata().created);
    const auto partial_path = final_path.native() + std::filesystem::path(".partial").native();
    archive_engine::CreateBackupRequest request;
    request.source_root = source;
    request.partial_path = partial_path;
    request.destination_path = final_path;
    request.excluded_root_names = {"Backup", "Work", "Bin"};
    request.metadata = metadata();
    std::uint64_t progress = 0;
    request.progress = [&](const std::uint64_t completed, const std::uint64_t,
                           const std::string&) { progress = completed; };

    const auto created = archive_engine::create_backup(request);
    if (!created.ok()) std::cerr << "Backup creation: " << created.detail << '\n';
    require(created.ok(), "create verified backup");
    require(std::filesystem::is_regular_file(final_path), "publish final ZIP");
    require(!std::filesystem::exists(partial_path), "remove partial on success");
    require(progress != 0, "publish streaming progress");

    const auto inspected = archive_engine::inspect_backup(final_path);
    require(inspected.result.ok(), "inspect verified backup");
    require(inspected.metadata.backup_type == "integration-test",
            "recover typed metadata");
    require(inspected.files.size() == 8, "manifest contains selected owner files");
    bool saw_legacy = false;
    for (const auto& file : inspected.files) {
        require(file.path.rfind("Backup/", 0) != 0, "exclude active Backup root");
        require(file.path.rfind("Work/", 0) != 0, "exclude Work root");
        require(file.path.rfind("Bin/", 0) != 0, "exclude Bin root");
        if (file.path == "Backups/legacy.txt") saw_legacy = true;
    }
    require(saw_legacy, "retain legacy plural backup material until migration");

    archive_engine::ExtractRequest extraction;
    extraction.archive_path = final_path;
    extraction.destination_root = root / "extracted";
    extraction.skip_backup_control_files = true;
    const auto extracted = archive_engine::extract_zip(extraction);
    require(extracted.ok(), "extract validated ZIP with create-new files");
    require(read_text(extraction.destination_root / "Config" / "SFX.INI") ==
                "alpha=1\n",
            "restore exact text content");
    require(read_text(extraction.destination_root / "Data" / "SFX.DB") ==
                std::string(131072, 'D'),
            "restore exact binary content");
    require(!std::filesystem::exists(extraction.destination_root / "Bin"),
            "program binaries never enter the content backup");
    require(!std::filesystem::exists(extraction.destination_root /
                                     archive_engine::backup_info_name),
            "skip backup control records during restore staging");

    const auto repeated = archive_engine::extract_zip(extraction);
    require(repeated.error == archive_engine::Error::destination_exists,
            "extraction never replaces an existing file");

    auto small_limits = archive_engine::Limits{};
    small_limits.maximum_total_bytes = 1024;
    require(archive_engine::inspect_zip(final_path, small_limits).result.error ==
                archive_engine::Error::limit_exceeded,
            "inspection enforces declared expansion limits");
}

void test_refusals_and_cleanup(const std::filesystem::path& root) {
    const auto unsafe = root / "unsafe.zip";
    create_raw_zip(unsafe, "../escape.txt", "escape");
    require(archive_engine::inspect_zip(unsafe).result.error ==
                archive_engine::Error::unsafe_path,
            "reject traversal before extraction");

    const auto absolute = root / "absolute.zip";
    create_raw_zip(absolute, "C:/escape.txt", "escape");
    require(archive_engine::inspect_zip(absolute).result.error ==
                archive_engine::Error::unsafe_path,
            "reject drive-qualified archive member");

    const auto source = root / "cancel-source";
    const auto backup = root / "cancel-backup";
    std::filesystem::create_directories(backup);
    write_text(source / "Data" / "SFX.DB", "data");
    archive_engine::CreateBackupRequest request;
    request.source_root = source;
    request.destination_path = backup / "cancelled.zip";
    request.partial_path = backup / "cancelled.zip.partial";
    request.metadata = metadata();
    request.cancelled = [] { return true; };
    const auto cancelled = archive_engine::create_backup(request);
    require(cancelled.error == archive_engine::Error::cancelled,
            "honor controlled cancellation");
    require(!std::filesystem::exists(request.partial_path) &&
                !std::filesystem::exists(request.destination_path),
            "controlled cancellation publishes nothing");
}

void test_generic_zip(const std::filesystem::path& root) {
    const auto source = root / "generic-source";
    const auto output = root / "generic.zip";
    write_text(source / "door" / "alpha.txt", "alpha\n");
    write_text(source / std::filesystem::u8path(u8"door/naïve-猫.txt"), "unicode\n");
    write_text(source / "door" / std::string(96, 'l') / "deep.txt", "long\n");
    write_text(source / "door" / "skip.log", "skip\n");
    write_text(source / "other.txt", "other\n");
    archive_engine::CreateZipRequest request;
    request.source_root = source;
    request.destination_path = output;
    request.partial_path = root / "generic.zip.partial";
    request.input_paths = {"door", "other.txt"};
    request.recursive = true;
    request.excluded_patterns = {"*.log"};
    request.compression_level = 9;
    const auto created = archive_engine::create_zip(request);
    if (!created.ok()) {
        throw std::runtime_error(std::string("create generic verified Zip64 archive: ") +
                                 archive_engine::error_name(created.error) + ": " +
                                 created.detail);
    }
    const auto inspection = archive_engine::inspect_zip(output);
    require(inspection.result.ok() && inspection.files.size() == 4,
            "inspect generic ZIP");
    require(inspection.files[0].path == "door/alpha.txt" &&
                inspection.files.back().path == "other.txt",
            "generic archive ordered Unicode/long-path selection");
    std::ifstream raw(output, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(raw),
                            std::istreambuf_iterator<char>()};
    require(bytes.find(std::string("PK\x06\x06", 4)) != std::string::npos,
            "generic archive emits Zip64 records without a giant fixture");

    archive_engine::ExtractRequest extraction;
    extraction.archive_path = output;
    extraction.destination_root = root / "generic-extract";
    extraction.included_patterns = {"door/*"};
    require(archive_engine::extract_zip(extraction).ok(),
            "extract selected generic ZIP content");
    require(read_text(extraction.destination_root / "door" / "alpha.txt") ==
                "alpha\n" &&
                !std::filesystem::exists(extraction.destination_root / "other.txt"),
            "generic extraction selection is bounded");

    request.destination_path = root / "nonrecursive.zip";
    request.partial_path = root / "nonrecursive.zip.partial";
    request.recursive = false;
    require(archive_engine::create_zip(request).error ==
                archive_engine::Error::invalid_request,
            "directory input requires explicit recursion");
}

void test_restore_transaction(const std::filesystem::path& root) {
    const auto installation = root / "restore-installation";
    for (const auto* role : {"Config", "Data", "Display", "Files", "Doors",
                             "Backup", "Logs", "Work", "Bin", "BotGate"}) {
        std::filesystem::create_directories(installation / role);
    }
    std::filesystem::create_directory(installation / "Backup" / "Upgrades");
    std::filesystem::create_directory(installation / "Backup" / "Scheduled");
    write_text(installation / "Config" / "SFX.conf", "state=A\n");
    write_text(installation / "Data" / "SFX.DB", "database=A-live\n");
    write_text(installation / "Data" / "SFX.DB-wal", "live-wal\n");
    write_text(installation / "Data" / "SFX.DB-shm", "live-shm\n");
    write_text(installation / "Data" / "SFXRuntime.lock", "runtime-only\n");
    write_text(installation / "Work" / "scheduler-database.snapshot",
               "database=A-snapshot\n");
    write_text(installation / "Display" / "welcome.ans", "display=A\n");
    write_text(installation / "Files" / "catalog.txt", "files=A\n");
    write_text(installation / "Data" / "FileIntake" / "held.payload", "held=A\n");
    write_text(installation / "Data" / "FileRemoved" / "removed.payload", "removed=A\n");
    write_text(installation / "Doors" / "door.cfg", "door=A\n");
    write_text(installation / "Logs" / "CALLERS.LOG", "log=A\n");
    write_text(installation / "Config" / "otherbbs.txt", "Name: Before\r\nTelnet: before.example:23\r\n\r\n");
    std::filesystem::create_directories(installation / "BotGate" / "can");
    write_text(installation / "BotGate" / "can" / "ip.can", "192.0.2.1\n");
    write_text(installation / "Bin" / "SFXRuntime.exe", "program-original\n");

    archive_engine::ManagedBackupRequest backup;
    backup.installation_root = installation;
    backup.kind = archive_engine::BackupKind::manual;
    backup.transaction_id = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    backup.source_generation = "7";
    backup.created = timestamp("20260914-083000");
    backup.database_snapshot = installation / "Work" / "scheduler-database.snapshot";
    const auto saved = archive_engine::create_managed_backup(backup);
    require(saved.result.ok(), "create managed data backup");
    require(saved.archive_path.parent_path() == installation / "Backup" / "Scheduled",
            "ordinary default backup selects Scheduled, never a flat archive");
    const auto saved_inspection = archive_engine::inspect_backup(saved.archive_path);
    require(saved_inspection.result.ok(), "inspect managed data backup");
    require(std::none_of(saved_inspection.files.begin(),saved_inspection.files.end(),[](const auto& file){return file.path.rfind("Files/",0)==0||file.path.rfind("Data/FileIntake/",0)==0||file.path.rfind("Data/FileRemoved/",0)==0;}),"core backup excludes in-tree library and durable intake/removed payloads by default");
    require(std::any_of(saved_inspection.files.begin(), saved_inspection.files.end(),
        [](const archive_engine::FileRecord& file) { return file.path == "Display/welcome.ans"; }),
        "new managed backup uses singular Display");
    require(std::none_of(saved_inspection.files.begin(), saved_inspection.files.end(),
                         [](const archive_engine::FileRecord& file) {
                             return file.path == "Data/SFXRuntime.lock";
                         }),
            "managed backup excludes process-lifetime Runtime lock");
    write_text(installation / "Config" / "SFX.conf", "state=B\n");
    write_text(installation / "Config" / "otherbbs.txt", "Name: After\r\nTelnet: after.example:23\r\n\r\n");
    write_text(installation / "BotGate" / "can" / "ip.can", "192.0.2.2\n");
    write_text(installation / "Data" / "SFX.DB", "database=B\n");
    write_text(installation / "Files" / "catalog.txt", "files=CURRENT\n");
    write_text(installation / "Data" / "FileIntake" / "held.payload", "held=CURRENT\n");
    write_text(installation / "Data" / "FileRemoved" / "removed.payload", "removed=CURRENT\n");
    const std::vector<std::filesystem::path> payload_names={installation/"Files"/"catalog.txt",installation/"Data"/"FileIntake"/"held.payload",installation/"Data"/"FileRemoved"/"removed.payload"};
    const std::vector<std::string> payload_bytes={"files=CURRENT\n","held=CURRENT\n","removed=CURRENT\n"};
#ifdef _WIN32
    struct PayloadLocks {std::vector<HANDLE> handles;~PayloadLocks(){for(auto h:handles)CloseHandle(h);}} payload_locks;
    std::vector<std::uint64_t> payload_ids;
    for(const auto& path:payload_names){auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);require(handle!=INVALID_HANDLE_VALUE,"retain real deny-write/rename/delete lease on current payload during every restore");payload_locks.handles.push_back(handle);BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(handle,&info)!=FALSE,"native payload identity");payload_ids.push_back((static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow);}
#endif
    const auto payloads_untouched=[&]{for(std::size_t n=0;n<payload_names.size();++n){require(read_text(payload_names[n])==payload_bytes[n],"core success/rollback/legacy restore leaves every current payload byte untouched");
#ifdef _WIN32
        const auto handle=CreateFileW(payload_names[n].c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);require(handle!=INVALID_HANDLE_VALUE,"same payload still at original name");BY_HANDLE_FILE_INFORMATION info{};const bool observed=GetFileInformationByHandle(handle,&info)!=FALSE;CloseHandle(handle);require(observed&&((static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow)==payload_ids[n],"core restore retains exact original NTFS payload objects");
#endif
    }};

    archive_engine::RestoreRequest restore;
    restore.installation_root = installation;
    restore.archive_path = saved.archive_path;
    restore.transaction_id = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    restore.sfx_version = SFX_PRODUCT_VERSION_SHORT_STRING;
    restore.source_generation = "8";
    restore.pre_restore_created = timestamp("20260914-083100");
    std::vector<std::filesystem::path> secured_directories;
    restore.secure_live_directory = [&](const std::filesystem::path& path) {
        secured_directories.push_back(path);
        return archive_engine::Result{};
    };
    std::vector<std::filesystem::path> secured_files;
    restore.secure_live_file = [&](const std::filesystem::path& path) {
        secured_files.push_back(path);
        return archive_engine::Result{};
    };
    unsigned prerequisite_calls{};
    restore.verify_live_publication=[&]{
        ++prerequisite_calls;
        require(read_text(installation/"Config"/"SFX.conf")=="state=A\n" && read_text(installation/"Data"/"SFX.DB")=="database=A-snapshot\n" && secured_files.size()==saved_inspection.files.size(),
            "new restore prerequisite runs after exact protected live publication, before commit");
        return archive_engine::Result{};
    };
    const auto restored = archive_engine::restore_backup(restore);
    if (!restored.result.ok()) std::cerr << "Restore diagnostic: " << restored.result.detail << '\n';
    require(restored.result.ok() && restored.live_state_changed &&
                !restored.rollback_attempted,
            "publish validated restore transaction");
    require(prerequisite_calls==1,"successful restore invokes current-store prerequisite once");
    payloads_untouched();
    require(read_text(installation / "Config" / "SFX.conf") == "state=A\n" &&
                read_text(installation / "Data" / "SFX.DB") ==
                    "database=A-snapshot\n" &&
                !std::filesystem::exists(installation / "Data" / "SFX.DB-wal") &&
                !std::filesystem::exists(installation / "Data" / "SFX.DB-shm"),
            "restore exact snapshot database without live SQLite sidecars");
    require(read_text(installation / "BotGate" / "can" / "ip.can")=="192.0.2.1\n","restore canonical BotGate rules exactly");
    require(std::find(secured_directories.begin(),secured_directories.end(),installation/"BotGate"/"can")!=secured_directories.end(),
        "restore protects nested mutable folders, not just seven role roots");
    require(read_text(installation / "Config" / "otherbbs.txt")=="Name: Before\r\nTelnet: before.example:23\r\n\r\n", "backup and restore authoritative Other BBS text exactly");
    require(read_text(restored.retained_previous_state / "Config" / "otherbbs.txt").find("Name: After")==0,"restore retains previous Other BBS text for recovery");
    require(read_text(installation / "Bin" / "SFXRuntime.exe") ==
                "program-original\n",
            "restore never promotes program binaries");
    require(read_text(restored.retained_previous_state / "Config" / "SFX.conf") ==
                "state=B\n" &&
                archive_engine::inspect_backup(restored.pre_restore_archive).result.ok(),
            "retain prior state and verified pre-restore archive");
    require(archive_engine::inspect_backup(restored.pre_restore_archive).metadata.sfx_version==SFX_PRODUCT_VERSION_SHORT_STRING,
            "pre-restore safety metadata retains actual trusted caller version instead of selected source archive version");
    require(restored.retained_previous_state.parent_path() == installation / "Backup" / "Upgrades" &&
                restored.pre_restore_archive.parent_path() == installation / "Backup" / "Upgrades",
            "restore recovery remains inside approved Upgrades destination");
    require(secured_files.size() == saved_inspection.files.size() &&
                std::all_of(secured_files.begin(), secured_files.end(),
                    [](const auto& path) { return std::filesystem::is_regular_file(path); }),
            "restore secures every published manifest file before reporting success");

    write_text(installation / "Config" / "SFX.conf", "state=C\n");
    write_text(installation / "Config" / "otherbbs.txt", "Name: Rollback\r\nTelnet: rollback.example:23\r\n\r\n");
    write_text(installation / "Data" / "SFX.DB", "database=C\n");
    restore.transaction_id = "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";
    restore.pre_restore_created = timestamp("20260914-083200");
    restore.secure_live_directory = [](const std::filesystem::path& path) {
        return path.filename() == "Data"
                   ? archive_engine::Result{archive_engine::Error::publication_failed,
                                            "injected directory-security failure"}
                   : archive_engine::Result{};
    };
    const auto refused = archive_engine::restore_backup(restore);
    require(!refused.result.ok() && refused.rollback_attempted &&
                refused.rollback_succeeded && !refused.live_state_changed,
            "failed restore reports successful rollback");
    require(read_text(installation / "Config" / "SFX.conf") == "state=C\n" &&
                read_text(installation / "Data" / "SFX.DB") == "database=C\n" &&
                !refused.retained_failed_state.empty(),
            "rollback restores every changed live role and retains failed state");
    require(read_text(installation / "Config" / "otherbbs.txt").find("Name: Rollback")==0,"failed restore rolls back Other BBS text with other data");
    payloads_untouched();
    restore.transaction_id = "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE";
    restore.pre_restore_created = timestamp("20260914-083210");
    restore.secure_live_directory = [](const std::filesystem::path& path) {
        return path.filename()=="can"
            ? archive_engine::Result{archive_engine::Error::publication_failed,"injected nested-directory security failure"}
            : archive_engine::Result{};
    };
    const auto nested_refused=archive_engine::restore_backup(restore);
    require(!nested_refused.result.ok()&&nested_refused.rollback_attempted&&nested_refused.rollback_succeeded&&!nested_refused.live_state_changed,
        "nested folder protection failure rolls back all published roles");
    require(prerequisite_calls==1,"early failed restore never verifies or clears a current-store prerequisite");
    payloads_untouched();
    restore.transaction_id="FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";restore.pre_restore_created=timestamp("20260914-083211");
    restore.secure_live_directory=[](const auto&){return archive_engine::Result{};};
    restore.verify_live_publication=[&]{write_text(installation/"Data"/"new-verification.state","required\n");return archive_engine::Result{archive_engine::Error::publication_failed,"controlled current-store prerequisite publication failure"};};
    const auto prerequisite_refused=archive_engine::restore_backup(restore);
    require(!prerequisite_refused.result.ok() && prerequisite_refused.rollback_attempted && prerequisite_refused.rollback_succeeded && !prerequisite_refused.live_state_changed &&
        read_text(installation/"Config"/"SFX.conf")=="state=C\n" && read_text(installation/"Data"/"SFX.DB")=="database=C\n" && !std::filesystem::exists(installation/"Data"/"new-verification.state") &&
        read_text(prerequisite_refused.retained_failed_state/"Data"/"new-verification.state")=="required\n","current-store prerequisite failure rolls back every live role and retains failed evidence");
    payloads_untouched();
    restore.verify_live_publication={};
#ifdef _WIN32
    // Fail AFTER at least one original Config entry moved. Rollback must not
    // move the later still-live original into failed-state and lose it live.
    write_text(installation/"Config"/"A-first.state","first-original\n");write_text(installation/"Config"/"Z-last.state","last-original\n");
    const auto locked=CreateFileW((installation/"Config"/"Z-last.state").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(locked!=INVALID_HANDLE_VALUE,"controlled late-name preservation lock");
    restore.transaction_id="11111111111111111111111111111111";restore.pre_restore_created=timestamp("20260914-083212");
    const auto partial=archive_engine::restore_backup(restore);CloseHandle(locked);
    require(!partial.result.ok()&&partial.rollback_attempted&&partial.rollback_succeeded&&!partial.live_state_changed&&read_text(installation/"Config"/"A-first.state")=="first-original\n"&&read_text(installation/"Config"/"Z-last.state")=="last-original\n"&&read_text(installation/"Config"/"SFX.conf")=="state=C\n","partial prior-state preservation rollback keeps both moved and never-moved originals live");
    require(!std::filesystem::exists(partial.retained_failed_state/"Config"/"Z-last.state"),"untouched original never reclassified as newly failed state");payloads_untouched();
#endif
    require(read_text(installation/"Config"/"SFX.conf")=="state=C\n"&&read_text(installation/"Data"/"SFX.DB")=="database=C\n",
        "nested folder failure preserves exact prior live data");

    // Old archives are immutable inputs. Map their legacy role only in staging,
    // and apply file security to the canonical published path, not its old name.
    const auto legacy_source = root / "legacy-display-source";
    write_text(legacy_source / "Displays" / "nonodes.asc", "legacy offline\r\n");
    write_text(legacy_source / "conf" / "otherbbs.txt", "Name: Legacy\r\nTelnet: legacy.example:23\r\n\r\n");
    write_text(legacy_source / "conf" / "extra.txt", "obsolete preserved in recovery\n");
    write_text(legacy_source/"Files"/"catalog.txt","legacy library must NOT replace current\n");
    write_text(legacy_source/"Data"/"FileIntake"/"held.payload","legacy intake must NOT replace current\n");
    write_text(legacy_source/"Data"/"FileRemoved"/"removed.payload","legacy removed must NOT replace current\n");
    archive_engine::CreateBackupRequest legacy;
    legacy.source_root = legacy_source;
    legacy.destination_path = root / "legacy-display.zip";
    legacy.partial_path = root / "legacy-display.partial";
    legacy.metadata = saved_inspection.metadata;
    legacy.metadata.created = timestamp("20260914-083000");
    const auto legacy_created = archive_engine::create_backup(legacy);
    if (!legacy_created.ok()) throw std::runtime_error("create legacy display archive: " + legacy_created.detail);
    restore.archive_path = legacy.destination_path;
    restore.transaction_id = "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD";
    restore.pre_restore_created = timestamp("20260914-083300");
    restore.secure_live_directory = [](const auto&) { return archive_engine::Result{}; };
    secured_files.clear();
    const auto mapped = archive_engine::restore_backup(restore);
    if (!mapped.result.ok()) std::cerr << "Legacy restore: " << mapped.result.detail << '\n';
    require(mapped.result.ok() && read_text(installation / "Display" / "nonodes.asc") ==
        "legacy offline\r\n" && !std::filesystem::exists(installation / "Displays"),
        "legacy display archive restores only into canonical Display");
    require(secured_files.size() == 2 && std::find(secured_files.begin(),secured_files.end(),installation / "Config" / "otherbbs.txt")!=secured_files.end(),
        "legacy text security callback uses canonical Config path");
    require(read_text(installation/"Config"/"otherbbs.txt").find("Name: Legacy")==0 && !std::filesystem::exists(installation/"conf"),"legacy backup maps list to Config without recreating conf");
    require(read_text(mapped.retained_previous_state/"Legacy-conf"/"extra.txt")=="obsolete preserved in recovery\n","obsolete archive contents retained outside live conf");
    require(archive_engine::inspect_backup(legacy.destination_path).result.ok(),
        "legacy source archive remains valid after restore");
    payloads_untouched();
    require(!std::filesystem::exists(mapped.retained_previous_state/"Files")&&!std::filesystem::exists(mapped.retained_previous_state/"Data"/"FileIntake")&&!std::filesystem::exists(mapped.retained_previous_state/"Data"/"FileRemoved"),"core restore never relocates current payload directories into recovery state");
    require(std::none_of(secured_files.begin(),secured_files.end(),[&](const auto& path){return std::find(payload_names.begin(),payload_names.end(),path)!=payload_names.end();}),"legacy payloads are not republished or passed to file-security mutation callbacks");

    write_text(legacy_source / "Display" / "nonodes.asc", "conflicting new offline\r\n");
    legacy.destination_path = root / "ambiguous-display.zip";
    legacy.partial_path = root / "ambiguous-display.partial";
    require(archive_engine::create_backup(legacy).ok(), "create ambiguous dual display archive");
    restore.archive_path = legacy.destination_path;
    restore.transaction_id = "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE";
    restore.pre_restore_created = timestamp("20260914-083400");
    const auto conflict = archive_engine::restore_backup(restore);
    require(!conflict.result.ok() && !conflict.live_state_changed &&
        conflict.pre_restore_archive.empty() &&
        read_text(installation / "Display" / "nonodes.asc") == "legacy offline\r\n",
        "dual display archive refused before changing live content or creating preservation archive");
}

void test_manifest_execute_permission(const std::filesystem::path& root) {
    const auto source=root/"mode-source";write_text(source/"Doors/native-door","#!/bin/sh\nexit 37\n");write_text(source/"Doors/data.txt","ordinary data\n");
#ifdef __linux__
    std::filesystem::permissions(source/"Doors/native-door",std::filesystem::perms::owner_all);
#endif
    archive_engine::CreateBackupRequest request;request.source_root=source;request.partial_path=root/"mode-backup.partial";request.destination_path=root/"mode-backup.zip";request.metadata=metadata();
    require(archive_engine::create_backup(request).ok(),"create native-mode backup");const auto inspected=archive_engine::inspect_backup(request.destination_path);require(inspected.result.ok(),"inspect native-mode backup");
    const auto program=std::find_if(inspected.files.begin(),inspected.files.end(),[](const auto& file){return file.path=="Doors/native-door";});require(program!=inspected.files.end(),"native program included");
#ifdef __linux__
    require(program->owner_executable,"Linux backup preserves source owner-execute evidence");
#else
    require(!program->owner_executable,"Windows backup retains legacy nonexecutable metadata");
#endif
    const auto info=archive_engine::read_zip_member(request.destination_path,archive_engine::backup_info_name,65536);require(info.result.ok(),"read original backup info");
    const auto manifest=archive_engine::read_zip_member(request.destination_path,archive_engine::backup_manifest_name,65536);require(manifest.result.ok(),"read original backup manifest");
    std::vector<archive_engine::FileRecord> records;require(archive_engine::decode_backup_manifest({manifest.bytes.begin(),manifest.bytes.end()},records),"decode original manifest");
    std::string extended="SFX-BACKUP-MANIFEST-2\n";for(const auto& row:records)extended+=row.path+'\t'+std::to_string(row.size)+'\t'+row.sha256+'\t'+(row.path=="Doors/native-door"?"1\n":"0\n");
    for(unsigned variant=0;variant<2;++variant){
        const auto zip=root/(variant?"mode-mismatch.zip":"mode-valid-v2.zip");auto* writer=archive_write_new();require(writer&&archive_write_set_format_zip(writer)==ARCHIVE_OK&&open_writer(writer,zip)==ARCHIVE_OK,"open controlled v2 ZIP");
        write_raw_entry(writer,archive_engine::backup_info_name,{info.bytes.begin(),info.bytes.end()});write_raw_entry(writer,archive_engine::backup_manifest_name,extended);
        for(const auto& row:inspected.files){const auto content=archive_engine::read_zip_member(request.destination_path,row.path,65536);require(content.result.ok(),"read original verified member");write_raw_entry(writer,row.path,{content.bytes.begin(),content.bytes.end()},AE_IFREG,row.path=="Doors/native-door"&&!variant?0700:0600);}
        require(archive_write_close(writer)==ARCHIVE_OK&&archive_write_free(writer)==ARCHIVE_OK,"close controlled mode ZIP");
        const auto result=archive_engine::inspect_backup(zip);require(variant?result.result.error==archive_engine::Error::manifest_invalid:result.result.ok(),"manifest execute bit must match exact ZIP member mode");
    }
    records.clear();require(archive_engine::decode_backup_manifest(extended,records),"v2 manifest parses on both hosts");
    auto malformed=extended;const auto bit=malformed.find("\t1\n");require(bit!=std::string::npos,"execute record present");malformed[bit+1]='2';records.clear();require(!archive_engine::decode_backup_manifest(malformed,records),"invalid execute bit refused");
}
void test_native_case_paths(const std::filesystem::path& root){
    const auto zip=root/"case-distinct.zip";auto* writer=archive_write_new();require(writer&&archive_write_set_format_zip(writer)==ARCHIVE_OK&&open_writer(writer,zip)==ARCHIVE_OK,"open case-distinct input ZIP");
    write_raw_entry(writer,"Doors/Game/NetMail/1.MSG","network A");write_raw_entry(writer,"Doors/Game/netmail/1.MSG","network B");
    require(archive_write_close(writer)==ARCHIVE_OK&&archive_write_free(writer)==ARCHIVE_OK,"close case-distinct input ZIP");const auto inspected=archive_engine::inspect_zip(zip);
#ifdef _WIN32
    require(!inspected.result.ok(),"Windows still refuses case-folding archive collisions");
#else
    require(inspected.result.ok()&&inspected.files.size()==2,"native case-sensitive archive entries remain distinct");
    archive_engine::ExtractRequest request;request.archive_path=zip;request.destination_root=root/"case-extracted";require(archive_engine::extract_zip(request).ok(),"extract both case-distinct native paths");
    require(read_text(request.destination_root/"Doors/Game/NetMail/1.MSG")=="network A"&&read_text(request.destination_root/"Doors/Game/netmail/1.MSG")=="network B","native extraction preserves both exact paths and bytes");
#endif
}

void test_source_change_diagnostics(const std::filesystem::path& root) {
    struct Scenario { const char* name; const char* replacement; const char* diagnostic; };
    const Scenario scenarios[] = {
        {"growth", "longer replacement", "source grew during archive creation"},
        {"shrink", "XYZ", "source size changed during archive creation"},
        {"hash", "OTHER", "source hash changed during archive creation"},
        {"escaped", "longer replacement", "source grew during archive creation"},
    };
    for (const auto& scenario : scenarios) {
        const auto source = root / (std::string("diagnostic-") + scenario.name);
        const auto member = std::string(scenario.name) == "escaped"
            ? std::string("Logs/\xCE\xA9.log") : std::string("Logs/live.log");
        const auto path = source / std::filesystem::u8path(member);
        write_text(path, "FIRST");
        archive_engine::CreateBackupRequest request;
        request.source_root = source;
        request.destination_path = root / (std::string("diagnostic-") + scenario.name + ".zip");
        request.partial_path = root / (std::string("diagnostic-") + scenario.name + ".partial");
        request.metadata = metadata();
        bool changed = false;
        request.backup_progress = [&](const auto& event) {
            if (!changed && event.stage == archive_engine::BackupStage::writing) {
                changed = true;
                write_text(path, scenario.replacement);
            }
        };
        const auto result = archive_engine::create_backup(request);
        require(changed && result.error == archive_engine::Error::source_changed,
                "changed source is refused with its source-change category");
        const auto shown_member = std::string(scenario.name) == "escaped"
            ? std::string("Logs/\\xCE\\xA9.log") : member;
        require(result.detail.find(scenario.diagnostic) != std::string::npos &&
                result.detail.find("file=\"" + shown_member + "\"") != std::string::npos &&
                result.detail.find("expected_bytes=5") != std::string::npos &&
                result.detail.find("read_bytes=" + std::to_string(std::string(scenario.replacement).size())) != std::string::npos,
                "diagnostic identifies safe relative member, exact reason and expected/read bytes");
        require(result.detail.find(scenario.replacement) == std::string::npos &&
                result.detail.find(source.u8string()) == std::string::npos,
                "diagnostic does not disclose file contents or absolute source directory");
        require(!std::filesystem::exists(request.destination_path) &&
                !std::filesystem::exists(request.partial_path) &&
                read_text(path) == scenario.replacement,
                "refused archive publishes nothing and preserves changed source bytes");
    }
}

#ifdef __linux__
void test_archive_writer_diagnostic(const std::filesystem::path& root) {
    const auto source = root / "writer-failure-source";
    write_text(source / "Data/payload.bin", std::string(262144, 'U'));
    archive_engine::CreateBackupRequest request;
    request.source_root = source;
    request.destination_path = root / "writer-failure.zip";
    request.partial_path = root / "writer-failure.partial";
    request.metadata = metadata();
    request.compression_level = 0;
    // Only this disposable child gets a small file-size limit. A real archive
    // write must fail while streaming a stored member; no product test hook.
    const auto child = fork();
    require(child >= 0, "start isolated archive output failure child");
    if (child == 0) {
        std::signal(SIGXFSZ, SIG_IGN);
        const rlimit limit{8192, 8192};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(3);
        const auto result = archive_engine::create_backup(request);
        const bool correct = result.error == archive_engine::Error::archive_io &&
            result.detail.find("cannot write source into archive") != std::string::npos &&
            result.detail.find("file=\"Data/payload.bin\"") != std::string::npos &&
            result.detail.find("archive_return=") != std::string::npos &&
            !std::filesystem::exists(request.destination_path) &&
            !std::filesystem::exists(request.partial_path);
        _exit(correct ? 0 : 4);
    }
    int status{};
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "real archive output failure stays distinct from source growth and names its member");
    require(std::filesystem::file_size(source / "Data/payload.bin") == 262144,
            "failed archive write preserves its source");
}

void test_coordinated_journal_namespace(const std::filesystem::path& parent,
    archive_engine::PhysicalLayout layout=archive_engine::PhysicalLayout::canonical) {
    const auto root=parent/(layout==archive_engine::PhysicalLayout::canonical?"journal-canonical":"journal-lowercase");
    std::filesystem::create_directories(root);
    const auto native=[&](std::string_view name){return std::filesystem::u8path(archive_engine::physical_relative(name,layout));};
    const std::string stamp = "20261116T010203004Z-0123456789ABCDEF0123456789ABCDEF.LOG";
    const std::vector<std::string> members = {
        "Logs/CALLERS.LOG", "Logs/HEYSYSOP.LOG", "Logs/SFXRuntime.log",
        "Logs/CALLERS-" + stamp, "Logs/HEYSYSOP-" + stamp, "Logs/SFXRuntime-" + stamp,
    };
    for (const auto& name : members)
        require(archive_engine::is_linux_journal_snapshot_member(name), "recognize exact active and rotated journal namespace");
    for (const auto* name : {"CALLERS.LOG", "logs/CALLERS.LOG", "Logs/sub/CALLERS.LOG",
                             "Logs/HEYSYSOP-invalid.LOG", "Logs/SFXRuntime.LOG",
                             "Logs/CALLERS.LOG/child", "Logs/CALLERS-20261116T010203004Z-not-a-nonce.LOG"})
        require(!archive_engine::is_linux_journal_snapshot_member(name), "do not widen captured journal namespace");
    const auto source = root / "journal-live";
    const auto snapshots = root / "journal-snapshots";
    archive_engine::CreateBackupRequest request;
    request.physical_layout=layout;
    request.source_root = source;
    request.destination_path = root / "journal-cut.zip";
    request.partial_path = root / "journal-cut.partial";
    request.metadata = metadata();
    request.linux_journal_snapshot_set = true;
    for (const auto& name : members) {
        write_text(source / native(name), "at capture: " + name);
        write_text(snapshots / native(name), "at capture: " + name);
        request.source_overrides.push_back({name, snapshots / native(name)});
    }
    write_text(source / native("Logs/notes.txt"), "ordinary notes");
    write_text(source / native("Logs/HEYSYSOP-invalid.LOG"), "unrelated filename");
    write_text(source / native("Logs/sub/CALLERS.LOG"), "unrelated nested file");
    // Writers have resumed after the host snapshot but before inventory starts.
    for (unsigned i = 0; i < 3; ++i) write_text(source / native(members[i]), "after capture: " + members[i]);
    require(std::filesystem::remove(source / native(members[3])), "simulate retention of a captured rotation");
    const auto new_rotation = "Logs/HEYSYSOP-20261116T010203005Z-FEDCBA9876543210FEDCBA9876543210.LOG";
    write_text(source / native(new_rotation), "created after capture");
    require(archive_engine::create_backup(request).ok(), "archive complete fixed journal snapshot after writers resume");
    const auto inspected = archive_engine::inspect_backup(request.destination_path);
    require(inspected.result.ok() && inspected.files.size() == members.size() + 3,
            "snapshot contains exact captured set plus ordinary unrelated logs");
    for (const auto& name : members) {
        const auto captured = archive_engine::read_zip_member(request.destination_path, name, 2048);
        require(captured.result.ok() && std::string(captured.bytes.begin(), captured.bytes.end()) == "at capture: " + name,
                "captured active and pruned rotation bytes survive unchanged");
    }
    require(std::none_of(inspected.files.begin(), inspected.files.end(), [&](const auto& item) { return item.path == new_rotation; }),
            "canonical rotation created after capture is excluded from the fixed journal set");
    request.destination_path = root / "journal-unrelated-change.zip";
    request.partial_path = root / "journal-unrelated-change.partial";
    bool changed = false;
    request.backup_progress = [&](const auto& event) {
        if (!changed && event.stage == archive_engine::BackupStage::writing) {
            changed = true;
            write_text(source / native("Logs/notes.txt"), "ordinary notes changed during backup");
        }
    };
    const auto refused = archive_engine::create_backup(request);
    require(changed && refused.error == archive_engine::Error::source_changed &&
            refused.detail.find("Logs/notes.txt") != std::string::npos,
            "snapshot mode retains integrity checks for unrelated live sources");
}
#endif
void test_lowercase_native_layout(const std::filesystem::path& root) {
    using Layout = archive_engine::PhysicalLayout;
    const auto lower = Layout::linux_lowercase_v1;
    const auto board = root / "lowercase-board";
    for (const auto* name : {"config", "data", "display", "doors", "logs", "botgate",
                             "backup/scheduled", "backup/upgrades", "work", "ftn"})
        std::filesystem::create_directories(board / name);
    const std::string marker = "SFX-LINUX-LAYOUT-1\nlinux-lowercase-v1\n";
    const std::string rotation = "Logs/CALLERS-20261116T010203005Z-0123456789ABCDEF0123456789ABCDEF.LOG";
    write_text(board / "config/sfx.conf", "before backup");
    write_text(board / "config/linux-layout.v1", marker);
    write_text(board / "data/sfx.db", "database snapshot bytes");
    write_text(board / "data/sfx.db.pre-restore", "retained database recovery bytes");
    write_text(board / "logs/callers.log", "caller records");
    write_text(board / "logs/heysysop.log", "attention records");
    write_text(board / "logs/sfxruntime.log", "runtime records");
    write_text(board / archive_engine::physical_relative(rotation, lower), "rotation records");
    write_text(board / "doors/kept-owner/DOOR.SYS", "required DOS filename");
    write_text(board / "ftn/inbound/owner-network/incoming.pkt", "internal inbound bytes");
    write_text(board / "ftn/external/OwnerNet/outbound/00000001.out", "external outbound bytes");
    write_text(board / "ftn/external/inbound/secure/queued.pkt", "external secure bytes");
    write_text(board / "ftn/external/OwnerNet/profile", "profile metadata");
    write_text(board / "ftn/external/OwnerNet/records/persist.rec", "record metadata");
    write_text(board / "ftn/external/runs/run-id/manifest", "run manifest metadata");
    write_text(board / "ftn/external/OwnerNet/profile.tmp", "preserved profile publication");
    write_text(board / "ftn/external/runs/run-id/manifest.tmp", "preserved manifest publication");
    write_text(board / "ftn/external/OwnerNet/outbound.003/00000001.pnt/00000002.flo", "three-digit zoned flow");
    write_text(board / "ftn/external/OwnerNet/outbound.1000/00000001.flo", "four-digit zoned flow");
    write_text(board / "data/fileintake/OwnerPayload", "durable intake");
    write_text(board / "data/fileremoved/OwnerPayload", "durable removed");
    archive_engine::ManagedBackupRequest backup;
    backup.physical_layout = lower;
    backup.installation_root = board;
    backup.kind = archive_engine::BackupKind::manual;
    backup.ftn_layout = true;
    backup.transaction_id = "0123456789ABCDEF0123456789ABCDEF";
    backup.sfx_version = SFX_PRODUCT_VERSION_SHORT_STRING;
    backup.created = timestamp("20261116-083000");
    const auto created = archive_engine::create_managed_backup(backup);
    require(created.result.ok(), "lowercase physical board creates a verified managed backup");
    require(created.archive_path.filename().u8string() == "sfx-backup-20261116-083000.zip",
            "Linux generated backup filename is lowercase");
    const auto inspection = archive_engine::inspect_backup(created.archive_path);
    require(inspection.result.ok() && inspection.metadata.sfx_version == SFX_PRODUCT_VERSION_SHORT_STRING,
            "lowercase board retains verified canonical archive metadata");
    const auto contains = [&](const std::string& name) {
        return std::any_of(inspection.files.begin(), inspection.files.end(),
                           [&](const auto& file) { return file.path == name; });
    };
    require(contains("Config/SFX.conf") && contains("Data/SFX.DB") && contains("Data/SFX.DB.pre-restore") &&
                contains("Logs/CALLERS.LOG") && contains("Logs/HEYSYSOP.LOG") &&
                contains("Logs/SFXRuntime.log") && contains(rotation),
            "physical lowercase names map to the standard core and journal namespace");
    require(contains("Doors/kept-owner/DOOR.SYS"), "required DOS filename remains exact");
    require(contains("FTN/Inbound/owner-network/incoming.pkt") &&
                contains("FTN/External/OwnerNet/Outbound/00000001.out") &&
                contains("FTN/External/Inbound/Secure/queued.pkt") &&
                contains("FTN/External/OwnerNet/Profile") && contains("FTN/External/OwnerNet/Records/persist.rec") &&
                contains("FTN/External/Runs/run-id/Manifest") &&
                contains("FTN/External/OwnerNet/Profile.tmp") && contains("FTN/External/Runs/run-id/Manifest.tmp") &&
                contains("FTN/External/OwnerNet/Outbound.003/00000001.pnt/00000002.flo") &&
                contains("FTN/External/OwnerNet/Outbound.1000/00000001.flo"),
            "owned FTN directory slots map while selected network codes remain exact");
    require(!contains("Data/FileIntake/OwnerPayload") && !contains("Data/FileRemoved/OwnerPayload"),
            "native lowercase durable intake and removed payload remain excluded from core backups");
    archive_engine::ExtractRequest extract;
    extract.archive_path = created.archive_path;
    extract.destination_root = root / "lowercase-extraction";
    extract.skip_backup_control_files = true;
    extract.physical_layout = lower;
    require(archive_engine::extract_zip(extract).ok(), "canonical members extract through explicit native mapping");
    require(read_text(extract.destination_root / "config/sfx.conf") == "before backup" &&
                read_text(extract.destination_root / "data/sfx.db") == "database snapshot bytes" &&
                read_text(extract.destination_root / "data/sfx.db.pre-restore") == "retained database recovery bytes" &&
                read_text(extract.destination_root / "ftn/external/OwnerNet/outbound/00000001.out") == "external outbound bytes" &&
                read_text(extract.destination_root / "ftn/external/runs/run-id/manifest") == "run manifest metadata" &&
                read_text(extract.destination_root / "ftn/external/OwnerNet/profile.tmp") == "preserved profile publication" &&
                read_text(extract.destination_root / "ftn/external/runs/run-id/manifest.tmp") == "preserved manifest publication" &&
                read_text(extract.destination_root / "ftn/external/OwnerNet/outbound.003/00000001.pnt/00000002.flo") == "three-digit zoned flow" &&
                read_text(extract.destination_root / "ftn/external/OwnerNet/outbound.1000/00000001.flo") == "four-digit zoned flow" &&
                read_text(extract.destination_root / "doors/kept-owner/DOOR.SYS") == "required DOS filename",
            "mapped extraction preserves all source bytes and DOS spelling");
    for (const auto& entry : std::filesystem::directory_iterator(extract.destination_root)) {
        const auto name = entry.path().filename().u8string();
        require(name == "config" || name == "data" || name == "doors" || name == "logs" || name == "ftn",
                "actual extracted owned root entries use lowercase spelling");
    }
    write_text(board / "config/sfx.conf", "changed after backup");
    archive_engine::RestoreRequest restore;
    restore.physical_layout = lower;
    restore.installation_root = board;
    restore.ftn_layout = true;
    restore.archive_path = created.archive_path;
    restore.transaction_id = "11111111ABCDEF0111111111ABCDEF01";
    restore.sfx_version = SFX_PRODUCT_VERSION_SHORT_STRING;
    restore.pre_restore_created = timestamp("20261116-083100");
    restore.secure_live_directory = [](const auto&) { return archive_engine::Result{}; };
    restore.secure_live_file = [](const auto&) { return archive_engine::Result{}; };
    const auto restored = archive_engine::restore_backup(restore);
    require(restored.result.ok() && restored.pre_restore_archive_created && restored.retained_previous_state_created,
            "native lowercase restore verifies preservation and publishes mapped roles");
    require(read_text(board / "config/sfx.conf") == "before backup" &&
                read_text(board / "data/fileintake/OwnerPayload") == "durable intake" &&
                read_text(board / "data/fileremoved/OwnerPayload") == "durable removed",
            "native restore changes core data and preserves durable file work");
    require(restored.retained_previous_state.filename().u8string() ==
                "restore-previous-11111111abcdef0111111111abcdef01" &&
                read_text(restored.retained_previous_state / "config/sfx.conf") == "changed after backup",
            "native retained recovery directory and its roles use lowercase actual names");
    write_text(board / "config/sfx.conf", "live state before rejected publication");
    restore.transaction_id = "22222222ABCDEF0122222222ABCDEF01";
    restore.pre_restore_created = timestamp("20261116-083200");
    restore.verify_live_publication = [] {
        return archive_engine::Result{archive_engine::Error::publication_failed, "controlled native publication refusal"};
    };
    const auto rolled_back = archive_engine::restore_backup(restore);
    require(!rolled_back.result.ok() && rolled_back.rollback_attempted && rolled_back.rollback_succeeded &&
                read_text(board / "config/sfx.conf") == "live state before rejected publication",
            "failed native publication rolls back exact current physical roles");
    require(rolled_back.retained_failed_state_created && rolled_back.retained_failed_state.filename().u8string() ==
                "restore-failed-22222222abcdef0122222222abcdef01",
            "native failed-state retention is lowercase without changing transaction identity");
    std::filesystem::remove(board / "config/linux-layout.v1");
    backup.created = timestamp("20261116-083300");
    const auto unmarked = archive_engine::create_managed_backup(backup);
    require(unmarked.result.ok(), "construct verified but unmarked archive for compatibility refusal");
    restore.archive_path = unmarked.archive_path;
    restore.transaction_id = "33333333ABCDEF0133333333ABCDEF01";
    restore.pre_restore_created = timestamp("20261116-083400");
    const auto refused = archive_engine::restore_backup(restore);
    require(refused.result.error == archive_engine::Error::metadata_invalid &&
                !refused.pre_restore_archive_created && !refused.live_state_changed &&
                read_text(board / "config/sfx.conf") == "live state before rejected publication",
            "unmarked old-layout restore refuses before preservation, staging or live changes");
    write_text(board / "config/linux-layout.v1", marker);
    backup.physical_layout = static_cast<Layout>(999);
    require(archive_engine::create_managed_backup(backup).result.error == archive_engine::Error::invalid_request,
            "unknown managed layout refuses before writes");
    restore.physical_layout = static_cast<Layout>(999);
    require(archive_engine::restore_backup(restore).result.error == archive_engine::Error::invalid_request,
            "unknown restore layout refuses before writes");
    extract.physical_layout = static_cast<Layout>(999);
    extract.destination_root = root / "invalid-layout-extraction";
    require(archive_engine::extract_zip(extract).error == archive_engine::Error::invalid_request &&
                !std::filesystem::exists(extract.destination_root),
            "unknown extraction layout creates no destination tree");
    require(archive_engine::physical_relative("Doors/OwnerGame/DOOR.SYS", lower) == "doors/OwnerGame/DOOR.SYS" &&
                archive_engine::archive_relative("doors/OwnerGame/DOOR.SYS", lower) == "Doors/OwnerGame/DOOR.SYS",
            "explicit mapping preserves arbitrary owner/game components");
}
} // namespace

int main(int argc, char** argv) {
    try {
#ifndef _WIN32
        // libarchive uses the process character locale for native POSIX paths.
        require(std::setlocale(LC_CTYPE, "") != nullptr, "host character locale is available");
        std::cout << "Archive test character locale: " << std::setlocale(LC_CTYPE, nullptr) << '\n';
#endif
        require(argc == 2 || (argc==3 && std::string(argv[2])=="--preserve"), "usage: archive_engine_tests <scratch-root> [--preserve]");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        require(root.filename() == "archive-engine", "refuse broad test cleanup");
        const bool preserve=argc==3;
        std::error_code error;
        if(preserve)require(!std::filesystem::exists(root),"fresh preserved archive fixture required");
        else std::filesystem::remove_all(root, error);
        require(!error, "clean exact archive test root");
        std::filesystem::create_directories(root);
        test_backup_round_trip(root);
        test_refusals_and_cleanup(root);
        test_generic_zip(root);
        test_restore_transaction(root);
        test_manifest_execute_permission(root);
        test_native_case_paths(root);
        test_lowercase_native_layout(root);
        test_source_change_diagnostics(root);
#ifdef __linux__
        test_archive_writer_diagnostic(root);
        test_coordinated_journal_namespace(root);
        test_coordinated_journal_namespace(root,archive_engine::PhysicalLayout::linux_lowercase_v1);
#endif
        if(!preserve)std::filesystem::remove_all(root, error);
        require(!error, "remove exact archive test root");
        std::cout << "ARCHIVE ENGINE PASS: "<<checks<<" checks; streaming Zip64 backup, metadata, "
                     "manifest, verified extraction, limits and refusals; "
                     "failed=0 skipped=0 exit 0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ARCHIVE ENGINE FAIL: " << error.what() << '\n';
        return 1;
    }
}
