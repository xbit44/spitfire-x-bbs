#include "sfx/archive/backup_service.hpp"
#if defined(__linux__)
#include "sfx/archive/managed_journal_snapshot.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sfx::archive {
namespace {

constexpr std::array<const char*, 6> restorable_roots{{
    "Config", "Data", "Display", "Doors", "Logs", "BotGate"}};
std::vector<const char*> restore_roles(bool ftn_layout) {
    std::vector<const char*> result(restorable_roots.begin(),restorable_roots.end());
    if(ftn_layout)result.push_back("FTN");
    return result;
}

bool equal_ascii(const std::string& a,const std::string& b) {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](unsigned char x,unsigned char y){return std::tolower(x)==std::tolower(y);});
}
bool payload_path(const std::string& path) {
    const auto slash=path.find('/');const auto root=path.substr(0,slash);
    if(equal_ascii(root,"Files")||equal_ascii(root,"FileContents"))return true;
    if(!equal_ascii(root,"Data")||slash==std::string::npos)return false;
    const auto end=path.find('/',slash+1);const auto child=path.substr(slash+1,end==std::string::npos?end:end-slash-1);
    return equal_ascii(child,"FileIntake")||equal_ascii(child,"FileRemoved");
}

Result failed(const Error error, std::string detail) {
    return Result{error, std::move(detail)};
}

bool transaction_id_valid(const std::string& value) {
    return value.size() == 32 &&
        std::all_of(value.begin(), value.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
        });
}

std::string backup_type(const BackupKind kind) {
    switch (kind) {
        case BackupKind::setup_upgrade: return "setup-upgrade";
        case BackupKind::manual: return "manual";
        case BackupKind::scheduled: return "scheduled";
        case BackupKind::pre_restore: return "pre-restore";
    }
    return {};
}

bool restorable_type(const std::string& value) {
    return value == "setup-upgrade" || value == "manual" ||
           value == "scheduled" || value == "pre-restore";
}

bool restorable_path(const std::string& path, bool ftn_layout) {
    const auto slash = path.find('/');
    const auto root = path.substr(0, slash);
    // Legacy Files members are valid INPUT, but are never core-restore output.
    return (ftn_layout && root == "FTN") || root == "FileContents" || root == "Files" || root == "Displays" || root == "conf" || std::any_of(restorable_roots.begin(), restorable_roots.end(),
        [&](const char* expected) { return root == expected; });
}

std::string canonical_restore_path(const std::string& path) {
    if(path=="conf/otherbbs.txt")return "Config/otherbbs.txt";
    return path.compare(0, 9, "Displays/") == 0 ? "Display/" + path.substr(9) : path;
}

bool safe_directory(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error) || error ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) ||
        error) return false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
#endif
    return true;
}

bool move_new(const std::filesystem::path& from,
              const std::filesystem::path& to) {
    std::error_code error;
    if (std::filesystem::exists(to, error) || error) return false;
    std::filesystem::rename(from, to, error);
    return !error;
}

bool move_directory_contents(const std::filesystem::path& from,
                             const std::filesystem::path& to,
                             const std::vector<std::filesystem::path>& excluded={},
                             std::vector<std::filesystem::path>* completed=nullptr) {
    if (!safe_directory(from) || !safe_directory(to)) return false;
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator iterator(from, error), end;
         !error && iterator != end; iterator.increment(error)) {
        entries.push_back(iterator->path().filename());
    }
    if (error) return false;
    std::sort(entries.begin(), entries.end());
    for (const auto& entry : entries) {
        if(std::any_of(excluded.begin(),excluded.end(),[&](const auto& name){return equal_ascii(name.u8string(),entry.u8string());}))continue;
        if (!move_new(from / entry, to / entry)) return false;
        if(completed)completed->push_back(entry);
    }
    return true;
}

bool remove_empty_tree(const std::filesystem::path& path) {
    std::error_code error;
    const auto count = std::filesystem::remove_all(path, error);
    return !error && count != static_cast<std::uintmax_t>(-1);
}

} // namespace

std::filesystem::path managed_backup_filename(const BackupKind kind,
                                              const Timestamp& timestamp,
                                              const PhysicalLayout physical_layout) {
    if(!valid_physical_layout(physical_layout))throw std::invalid_argument("invalid managed backup physical layout");
    const char* prefix = "SFX-backup-";
    if (kind == BackupKind::setup_upgrade) prefix = "SFX-setup-backup-";
    else if (kind == BackupKind::scheduled) prefix = "SFX-scheduled-backup-";
    else if (kind == BackupKind::pre_restore) prefix = "SFX-pre-restore-backup-";
    const auto name=std::string(prefix)+timestamp.filename_component+".zip";
    return std::filesystem::u8path(physical_layout==PhysicalLayout::linux_lowercase_v1
        ?layout_detail::lower_owned(name):name);
}

ManagedBackupResult create_managed_backup(const ManagedBackupRequest& request) {
    ManagedBackupResult result;
    if (!valid_physical_layout(request.physical_layout)) {
        result.result=failed(Error::invalid_request,"invalid managed backup physical layout");return result;
    }
    const auto physical=[&](std::string_view name){return std::filesystem::u8path(physical_relative(name,request.physical_layout));};
    if (!transaction_id_valid(request.transaction_id) ||
        !safe_directory(request.installation_root) ||
        !safe_directory(request.installation_root / physical("Backup"))) {
        result.result = failed(Error::invalid_request,
                               "invalid installation root, Backup directory or transaction");
        return result;
    }
    const auto created = request.created.filename_component.empty()
                             ? current_timestamp()
                             : request.created;
    const auto destination = request.destination_directory.empty()
        ? request.installation_root / physical(
            request.kind == BackupKind::setup_upgrade || request.kind == BackupKind::pre_restore
                ? "Backup/Upgrades" : "Backup/Scheduled") : request.destination_directory;
    if (!safe_directory(destination)) {
        result.result = failed(Error::unsafe_path, "backup destination is unavailable or unsafe; no fallback");
        return result;
    }
    result.archive_path = destination /
                          managed_backup_filename(request.kind, created,request.physical_layout);
    CreateBackupRequest archive_request;
    archive_request.physical_layout=request.physical_layout;
    archive_request.source_root = request.installation_root;
    archive_request.destination_path = result.archive_path;
    archive_request.partial_path = result.archive_path.native() +
                                   std::filesystem::path(".partial").native();
    archive_request.included_root_names.assign(restorable_roots.begin(),
                                               restorable_roots.end());
    if(request.ftn_layout) {
        if(!safe_directory(request.installation_root/physical("FTN"))) {
            result.result=failed(Error::unsafe_path,"verified FTN role unavailable; backup refused");
            return result;
        }
        archive_request.included_root_names.push_back("FTN");
    }
    // Transition backups retain obsolete contents without recreating conf.
    archive_request.included_root_names.push_back("conf");
    // Runtime owns this process-lifetime lock with exclusive sharing.  It is
    // neither persistent BBS data nor valid restore input, so a live backup
    // must never try to hash or archive it.
    archive_request.excluded_relative_paths = {"Data/SFXRuntime.lock","Data/FileIntake","Data/FileRemoved"};
#if defined(__linux__)
    archive_request.excluded_relative_paths.push_back("Config/.file-identities-linux.lock");
#endif
    // A configured destination may not be recursively captured even if a
    // lower-level caller places it inside one of the mutable source roles.
    const auto relative_destination = destination.lexically_relative(request.installation_root);
    if (!relative_destination.empty() && *relative_destination.begin() != "..")
        archive_request.excluded_relative_paths.push_back(archive_relative(relative_destination.generic_u8string(),request.physical_layout));
    if (!request.database_snapshot.empty()) {
        archive_request.excluded_relative_paths.insert(
            archive_request.excluded_relative_paths.end(),
            {"Data/SFX.DB", "Data/SFX.DB-wal", "Data/SFX.DB-shm"});
        archive_request.source_overrides.push_back(
            {"Data/SFX.DB", request.database_snapshot});
    }
    archive_request.metadata.backup_type = backup_type(request.kind);
    if(!request.portable_identity_snapshot.empty()){
        archive_request.excluded_relative_paths.push_back("Config/file-identities.v1");
        archive_request.source_overrides.push_back({"Config/file-identities.v1",request.portable_identity_snapshot});
    }
    if(!request.linux_portable_identity_snapshot.empty()){
#if defined(__linux__)
        archive_request.excluded_relative_paths.push_back("Config/file-identities-linux.v1");
        archive_request.source_overrides.push_back({"Config/file-identities-linux.v1",request.linux_portable_identity_snapshot});
#else
        result.result=failed(Error::invalid_request,"Linux identity snapshot requires native Linux host");return result;
#endif
    }
#if defined(__linux__)
    if(request.linux_journal_snapshot_set) {
        if(request.database_snapshot.empty()||!request.operational_log_snapshot.empty()||
           request.linux_journal_snapshots.size()<2||request.linux_journal_snapshots.size()>771) {
            result.result=failed(Error::invalid_request,"coordinated journal capture requires database and a complete bounded journal set");return result;
        }
        std::set<std::string> names;
        std::set<std::filesystem::path> native_names;
        const auto work=(request.installation_root/physical("Work")).lexically_normal();
        for(const auto& source:request.linux_journal_snapshots) {
            const auto native=source.native_path.lexically_normal();
            if(!is_linux_journal_snapshot_member(source.archive_path)||
               native.parent_path()!=work||native.filename().empty()||
               !names.insert(source.archive_path).second||!native_names.insert(native).second) {
                result.result=failed(Error::invalid_request,"invalid coordinated journal snapshot member or private source");return result;
            }
        }
        if(!names.count("Logs/CALLERS.LOG")||!names.count("Logs/HEYSYSOP.LOG")) {
            result.result=failed(Error::invalid_request,"coordinated journal capture is missing an active durable journal");return result;
        }
        archive_request.linux_journal_snapshot_set=true;
        archive_request.source_overrides.insert(archive_request.source_overrides.end(),
            request.linux_journal_snapshots.begin(),request.linux_journal_snapshots.end());
    } else if(!request.linux_journal_snapshots.empty()) {
        result.result=failed(Error::invalid_request,"journal snapshot members require a coordinated capture boundary");return result;
    }
    if(!request.operational_log_snapshot.empty()){
        archive_request.excluded_relative_paths.push_back("Logs/SFXRuntime.log");
        archive_request.source_overrides.push_back({"Logs/SFXRuntime.log",request.operational_log_snapshot});
    }
#endif
    for(const auto& source:request.file_contents){
        if(source.archive_path.rfind("FileContents/",0)!=0){result.result=failed(Error::invalid_request,"file content override outside reserved namespace");return result;}
        archive_request.source_overrides.push_back(source);
    }
    archive_request.metadata.transaction_id = request.transaction_id;
    archive_request.metadata.sfx_version = request.sfx_version;
    archive_request.metadata.source_generation = request.source_generation;
    archive_request.metadata.created = created;
    archive_request.limits = request.limits;
    archive_request.cancelled = request.cancelled;
    archive_request.progress = request.progress;
    result.result = create_backup(archive_request);
    return result;
}

RestoreResult restore_backup(const RestoreRequest& request) {
    RestoreResult result;
    if(!valid_physical_layout(request.physical_layout)){
        result.result=failed(Error::invalid_request,"invalid restore physical layout");return result;
    }
    const auto physical=[&](std::string_view name){return std::filesystem::u8path(physical_relative(name,request.physical_layout));};
    const auto owned=[&](const std::string& name){return std::filesystem::u8path(
        request.physical_layout==PhysicalLayout::linux_lowercase_v1?layout_detail::lower_owned(name):name);};
    if (!transaction_id_valid(request.transaction_id) ||
        !safe_directory(request.installation_root) ||
        !safe_directory(request.installation_root / physical("Backup")) ||
        !safe_directory(request.installation_root / physical("Work"))) {
        result.result = failed(Error::invalid_request,
                               "invalid installation root or restore transaction");
        return result;
    }
#ifdef _WIN32
    if (!request.secure_live_directory || !request.secure_live_file) {
        result.result = failed(Error::invalid_request,
                               "Windows restore requires live directory and file security callbacks");
        return result;
    }
#endif

    const auto inspection = inspect_backup(request.archive_path, request.limits);
    if (!inspection.result.ok()) {
        result.result = inspection.result;
        return result;
    }
    if (!restorable_type(inspection.metadata.backup_type) ||
        !std::all_of(inspection.files.begin(), inspection.files.end(),
                     [&](const FileRecord& file) { return restorable_path(file.path,request.ftn_layout); })) {
        result.result = failed(Error::metadata_invalid,
                               "archive is not a restorable SFX data backup");
        return result;
    }

    const bool has_legacy_display = std::any_of(inspection.files.begin(), inspection.files.end(),
        [](const FileRecord& file) { return file.path.compare(0, 9, "Displays/") == 0; });
    const bool has_display = std::any_of(inspection.files.begin(), inspection.files.end(),
        [](const FileRecord& file) { return file.path.compare(0, 8, "Display/") == 0; });
    if (has_legacy_display && has_display) {
        result.result = failed(Error::metadata_invalid,
                               "ambiguous Display and Displays trees; live data unchanged");
        return result;
    }
    const auto contains=[&](const std::string& path) {
        return std::any_of(inspection.files.begin(),inspection.files.end(),[&](const auto& file){return file.path==path;});
    };
    if(contains("conf/otherbbs.txt") && contains("Config/otherbbs.txt")) {
        result.result=failed(Error::metadata_invalid,"ambiguous legacy and current Other BBS files; live data unchanged");
        return result;
    }
    if(request.physical_layout==PhysicalLayout::linux_lowercase_v1){
        const auto marker=read_zip_member(request.archive_path,"Config/linux-layout.v1",128);
        const std::string expected="SFX-LINUX-LAYOUT-1\nlinux-lowercase-v1\n";
        if(!marker.result.ok()||std::string(marker.bytes.begin(),marker.bytes.end())!=expected||
           has_legacy_display||std::any_of(inspection.files.begin(),inspection.files.end(),
             [](const auto& file){return file.path.rfind("conf/",0)==0;})){
            result.result=failed(Error::metadata_invalid,"restore_requires_matching_linux_lowercase_layout");return result;
        }
    }

    ManagedBackupRequest preservation;
    preservation.physical_layout=request.physical_layout;
    preservation.installation_root = request.installation_root;
    preservation.ftn_layout = request.ftn_layout;
    preservation.destination_directory = request.installation_root / physical("Backup/Upgrades");
    preservation.kind = BackupKind::pre_restore;
    preservation.transaction_id = request.transaction_id;
    preservation.sfx_version = request.sfx_version;
    preservation.source_generation = request.source_generation;
    preservation.created = request.pre_restore_created;
    preservation.limits = request.limits;
    preservation.cancelled = request.cancelled;
    preservation.progress = request.progress;
    const auto preserved = create_managed_backup(preservation);
    result.pre_restore_archive = preserved.archive_path;
    if (!preserved.result.ok()) {
        result.result = preserved.result;
        return result;
    }
    result.pre_restore_archive_created = true;

    const auto transaction = request.installation_root / physical("Work") /
        owned("restore-" + request.transaction_id);
    const auto stage = transaction / "stage";
    const auto previous = transaction / "previous";
    const auto failed_state = transaction / "failed";
    std::error_code error;
    if (std::filesystem::exists(transaction, error) || error ||
        !std::filesystem::create_directories(stage, error) || error ||
        !std::filesystem::create_directory(previous, error) || error) {
        result.result = failed(Error::destination_exists,
                               "restore staging transaction already exists or cannot be created");
        return result;
    }

    ExtractRequest extraction;
    extraction.physical_layout=request.physical_layout;
    extraction.archive_path = request.archive_path;
    extraction.destination_root = stage;
    extraction.limits = request.limits;
    extraction.skip_backup_control_files = true;
    // Read/verify old manifests normally, but NEVER extract/publish their file
    // payloads as a side effect of restoring settings or catalog metadata.
    extraction.excluded_patterns={"FileContents","FileContents/*","Files","Files/*","Data/FileIntake","Data/FileIntake/*","Data/FileRemoved","Data/FileRemoved/*"};
    extraction.cancelled = request.cancelled;
    extraction.progress = request.progress;
    result.result = extract_zip(extraction);
    if (!result.result.ok()) {
        remove_empty_tree(transaction);
        return result;
    }
    if (has_legacy_display &&
        (!safe_directory(stage / "Displays") || !move_new(stage / "Displays", stage / "Display"))) {
        result.result = failed(Error::unsafe_path, "cannot map legacy display directory safely");
        remove_empty_tree(transaction);
        return result;
    }
    // Map the old authoritative file into Config. Keep all other obsolete
    // contents in retained recovery state, never publish a new live conf tree.
    if(contains("conf/otherbbs.txt")) {
        std::filesystem::create_directories(stage/"Config",error);
        if(error || !safe_directory(stage/"conf") || !safe_directory(stage/"Config") ||
            !move_new(stage/"conf"/"otherbbs.txt",stage/"Config"/"otherbbs.txt")) {
            result.result=failed(Error::unsafe_path,"cannot map legacy Other BBS file safely");
            return result;
        }
    }
    if(std::filesystem::exists(stage/"conf",error) &&
        (!safe_directory(stage/"conf") || !move_new(stage/"conf",previous/"Legacy-conf"))) {
        result.result=failed(Error::unsafe_path,"cannot retain legacy conf recovery contents");
        return result;
    }
    for (const char* role : restore_roles(request.ftn_layout)) {
        const auto staged_role = stage / physical(role);
        if (!std::filesystem::exists(staged_role, error)) {
            error.clear();
            if (!std::filesystem::create_directory(staged_role, error) || error) {
                result.result = failed(Error::archive_io,
                                       "cannot complete empty restore role");
                remove_empty_tree(transaction);
                return result;
            }
        }
        if (!safe_directory(staged_role) ||
            !safe_directory(request.installation_root / physical(role))) {
            result.result = failed(Error::unsafe_path,
                                   "unsafe live or staged restore role");
            remove_empty_tree(transaction);
            return result;
        }
    }

    struct RoleMove {
        std::string name;
        std::vector<std::filesystem::path> excluded,original,preserved;
    };
    std::vector<RoleMove> moved;
    auto rollback = [&] {
        result.rollback_attempted = true;
        bool ok = true;
        std::filesystem::create_directory(failed_state, error);
        if (error) ok = false;
        for (auto iterator = moved.rbegin(); iterator != moved.rend(); ++iterator) {
            const auto live = request.installation_root / iterator->name;
            const auto old = previous / iterator->name;
            const auto failed_live = failed_state / iterator->name;
            error.clear();
            if (!std::filesystem::create_directory(failed_live, error) || error)
                ok = false;
            error.clear();
            // A preservation move may have failed partway through a role.
            // Keep originals that NEVER moved in place; only newly published
            // or callback-created state belongs in the failed-state tree.
            auto untouched=iterator->excluded;
            for(const auto& name:iterator->original)
                if(std::find(iterator->preserved.begin(),iterator->preserved.end(),name)==iterator->preserved.end())untouched.push_back(name);
            if (!move_directory_contents(live, failed_live,untouched)) ok = false;
            if (!move_directory_contents(old, live)) ok = false;
        }
        result.rollback_succeeded = ok;
        result.live_state_changed = !ok;
        if (std::filesystem::exists(failed_state, error) && !error) {
            result.retained_failed_state = request.installation_root / physical("Backup/Upgrades") /
                owned("Restore-Failed-" + request.transaction_id);
            if (!move_new(failed_state, result.retained_failed_state)) ok = false;
            else result.retained_failed_state_created = true;
        }
        result.rollback_succeeded = ok;
    };

    for (const char* role : restore_roles(request.ftn_layout)) {
        const auto native_role=physical(role);
        const auto live = request.installation_root / native_role;
        const auto old = previous / native_role;
        error.clear();
        if (!std::filesystem::create_directory(old, error) || error) {
            result.result = failed(Error::publication_failed,
                                   "cannot create prior-state restore role");
            rollback();
            return result;
        }
        RoleMove journal;journal.name=native_role.generic_u8string();
        if(std::string_view(role)=="Data")journal.excluded={physical("Data/FileIntake").filename(),physical("Data/FileRemoved").filename()};
        for(std::filesystem::directory_iterator entry(live,error),end;!error&&entry!=end;entry.increment(error))journal.original.push_back(entry->path().filename());
        if(error){result.result=failed(Error::source_io,"cannot inventory prior-state restore role");rollback();return result;}
        moved.push_back(std::move(journal));
        if (!move_directory_contents(live, old,moved.back().excluded,&moved.back().preserved)) {
            result.result = failed(Error::publication_failed,
                                   "cannot preserve live restore role contents");
            rollback();
            return result;
        }
        if (!move_directory_contents(stage / native_role, live,moved.back().excluded)) {
            result.result = failed(Error::publication_failed,
                                   "cannot publish staged restore role contents");
            rollback();
            return result;
        }
        result.live_state_changed = true;
        if (request.secure_live_directory) {
            const auto secured = request.secure_live_directory(live);
            if (!secured.ok()) {
                result.result = secured;
                rollback();
                return result;
            }
        }
    }
    // Extracted parent folders inherit staging permissions. Protect every
    // manifest-derived nested folder as live mutable data, parent before child,
    // before declaring a restore successful. Never repair an unrelated tree.
    std::set<std::string> nested_directories;
    for (const auto& file : inspection.files) {
        if(payload_path(file.path))continue;
        if (file.path.compare(0, 5, "conf/") == 0 && file.path != "conf/otherbbs.txt") continue;
        auto relative = physical(canonical_restore_path(file.path)).parent_path();
        while (!relative.empty() && relative.has_parent_path()) {
            nested_directories.insert(relative.generic_u8string());
            relative = relative.parent_path();
        }
    }
    for (const auto& relative : nested_directories) {
        const auto live_directory = request.installation_root / std::filesystem::u8path(relative);
        if (!safe_directory(live_directory)) {
            result.result = failed(Error::unsafe_path, "restored nested directory is unavailable or unsafe");
            rollback();
            return result;
        }
        const auto secured = request.secure_live_directory(live_directory);
        if (!secured.ok()) {
            result.result = secured;
            rollback();
            return result;
        }
    }
    if (request.secure_live_file) {
        for (const auto& file : inspection.files) {
            if(payload_path(file.path))continue;
            if(file.path.compare(0,5,"conf/")==0 && file.path!="conf/otherbbs.txt")continue;
            const auto live_file = request.installation_root /
                physical(canonical_restore_path(file.path));
            error.clear();
            if (!std::filesystem::is_regular_file(live_file, error) || error) {
                result.result = failed(Error::publication_failed,
                                       "restored manifest file is unavailable");
                rollback();
                return result;
            }
            const auto secured = request.secure_live_file(live_file);
            if (!secured.ok()) {
                result.result = secured;
                rollback();
                return result;
            }
        }
    }

    if(request.verify_live_publication){
        Result verified;
        try{verified=request.verify_live_publication();}
        catch(...){verified=failed(Error::publication_failed,"restored live prerequisite could not be confirmed");}
        if(!verified.ok()){result.result=verified;rollback();return result;}
    }
    result.retained_previous_state = request.installation_root / physical("Backup/Upgrades") /
        owned("Restore-Previous-" + request.transaction_id);
    if (!move_new(previous, result.retained_previous_state)) {
        result.result = failed(Error::publication_failed,
                               "restore completed but prior-state retention failed");
        rollback();
        return result;
    }
    result.retained_previous_state_created = true;
    result.live_state_changed = true;
    result.result = {};
    remove_empty_tree(transaction);
    return result;
}

} // namespace sfx::archive
