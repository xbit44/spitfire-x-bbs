#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/application/scheduler.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <sstream>
#include <set>

namespace sfx::platform::win32 {
namespace {
namespace fs = std::filesystem;
namespace inst = installation;
constexpr std::size_t maximum_members = 4096;

bool beneath(const fs::path& child, const fs::path& parent) {
    const auto c = child.native(), p = parent.native();
    return !_wcsicmp(c.c_str(), p.c_str()) ||
        (c.size() > p.size() && c[p.size()] == L'\\' && !_wcsnicmp(c.c_str(), p.c_str(), p.size()));
}

bool missing(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    inst::require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
        "backup_destination_unavailable_no_fallback");
    return true;
}

std::string hash_handle(HANDLE file, const archive::CancelCheck& cancelled = {}) {
    LARGE_INTEGER zero{};
    inst::require(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE, "backup_hash_seek_failed");
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    inst::require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
        "backup_hash_provider_failed");
    struct Cleanup {
        BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
        ~Cleanup() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } cleanup{algorithm, hash};
    inst::require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0,
        "backup_hash_create_failed");
    std::array<unsigned char, 65536> buffer{}; DWORD bytes{};
    for (;;) {
        inst::require(!cancelled || !cancelled(), "backup_retention_cancelled_preserved");
        inst::require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr) != FALSE,
            "backup_hash_read_failed");
        if (!bytes) break;
        inst::require(BCryptHashData(hash, buffer.data(), bytes, 0) == 0, "backup_hash_failed");
    }
    std::array<unsigned char, 32> digest{};
    inst::require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0,
        "backup_hash_finish_failed");
    constexpr char hex[] = "0123456789ABCDEF"; std::string result;
    for (const auto ch : digest) { result.push_back(hex[ch >> 4]); result.push_back(hex[ch & 15]); }
    return result;
}

const char* pool_type(const archive::BackupKind kind) {
    if (kind == archive::BackupKind::setup_upgrade) return "setup-upgrade";
    inst::require(kind == archive::BackupKind::manual || kind == archive::BackupKind::scheduled,
        "backup_retention_kind_invalid");
    return "ordinary";
}

bool type_matches(const std::string& type, const archive::BackupKind kind) {
    return kind == archive::BackupKind::setup_upgrade ? type == "setup-upgrade" :
        type == "manual" || type == "scheduled";
}

struct Member {
    fs::path path;
    std::uint64_t sequence{}, file_id{}, marker_id{};
    DWORD volume{};
    std::string transaction, digest, marker_digest;
    archive::BackupInspection inspection;
    std::vector<inst::Handle> pins;
};

std::optional<Member> managed_member(const fs::path& path, const std::wstring& owner,
    const std::string& installation_id, const archive::BackupKind kind, const archive::CancelCheck& cancelled = {},
    const archive::Limits* limits = nullptr) {
    // Anything not strictly recorded and verified stays untouched. A malformed
    // sidecar/foreign installation/old flat archive is never a deletion permit.
    try {
        WindowsStorageDaclPolicy policy(owner);
        const auto marker_path = fs::path(path.native() + L".sfx-owned");
        auto marker = inst::open_object(marker_path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
        if (!policy.assess(marker_path, StorageDaclObjectKind::file).status) return std::nullopt;
        const auto raw = inst::read_file(marker_path, 4096);
        std::istringstream input(std::string(raw.begin(), raw.end()));
        std::array<std::string, 7> fields{};
        for (auto& field : fields) if (!std::getline(input, field)) return std::nullopt;
        if (input.peek() != std::char_traits<char>::eof() || fields[0] != "SFX-BACKUP-OWNER-1" ||
            fields[1] != installation_id || fields[2] != pool_type(kind) ||
            fields[6] != path.filename().u8string()) return std::nullopt;
        std::uint64_t sequence{};
        const auto number = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), sequence);
        if (number.ec != std::errc{} || number.ptr != fields[3].data() + fields[3].size() || !sequence)
            return std::nullopt;
        auto file = inst::open_object(path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
        if (!policy.assess(path, StorageDaclObjectKind::file).status || hash_handle(file.get(), cancelled) != fields[5])
            return std::nullopt;
        auto checked = archive::inspect_backup(path,limits ? *limits : archive::Limits{},cancelled);
        if (!checked.result.ok() || !type_matches(checked.metadata.backup_type, kind) ||
            checked.metadata.transaction_id != fields[4]) return std::nullopt;
        // Inspection intentionally clears the filename-only timestamp field.
        // Reconstruct it strictly from the recorded civil timestamp, not from
        // the candidate filename or an empty inspection-only placeholder.
        auto timestamp = checked.metadata.created;
        const auto& civil = timestamp.local;
        if (civil.size() != 19 || civil[4] != '-' || civil[7] != '-' ||
            civil[10] != 'T' || civil[13] != ':' || civil[16] != ':') return std::nullopt;
        for (std::size_t i = 0; i < civil.size(); ++i)
            if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 &&
                (civil[i] < '0' || civil[i] > '9')) return std::nullopt;
        timestamp.filename_component = civil.substr(0, 4) + civil.substr(5, 2) + civil.substr(8, 2) +
            "-" + civil.substr(11, 2) + civil.substr(14, 2) + civil.substr(17, 2);
        const auto expected = archive::managed_backup_filename(
            checked.metadata.backup_type == "setup-upgrade" ? archive::BackupKind::setup_upgrade :
            checked.metadata.backup_type == "manual" ? archive::BackupKind::manual : archive::BackupKind::scheduled,
            timestamp);
        if (expected != path.filename()) return std::nullopt;
        Member member{path, sequence, inst::file_id(file.get()), inst::file_id(marker.get()),
            inst::volume_serial(file.get()), fields[4], fields[5], inst::sha256(raw)};
        member.inspection=std::move(checked);
        member.pins.push_back(std::move(file));member.pins.push_back(std::move(marker));
        return member;
    } catch (const std::exception&) { return std::nullopt; }
}
VerifiedBackupMember take_verified(Member& member){
    // Reuse is local to this operation and these continuously held deny-write/
    // delete object leases. No timestamp cache or trust across operations.
    VerifiedBackupMember item;item.path=member.path;item.sequence=member.sequence;item.sha256=member.digest;
    item.pins=std::move(member.pins);item.inspection=std::move(member.inspection);
    item.contents=archive::inspect_content_index(item.path,item.inspection);return item;
}

void delete_exact(const fs::path& path, const std::uint64_t expected_id,
    const DWORD volume, const std::wstring& owner, const std::string& expected_digest,
    const archive::CancelCheck& cancelled) {
    // Reopen with DELETE access, compare the exact object and stream digest
    // again under deny-write/delete sharing. Delete by held handle, not name.
    auto file = inst::open_object(path, false, GENERIC_READ | READ_CONTROL | DELETE, FILE_SHARE_READ);
    inst::require(inst::file_id(file.get()) == expected_id && inst::volume_serial(file.get()) == volume &&
        WindowsStorageDaclPolicy(owner).assess(path, StorageDaclObjectKind::file).status.succeeded() &&
        hash_handle(file.get(), cancelled) == expected_digest, "backup_retention_member_changed_preserved");
    FILE_DISPOSITION_INFO disposition{TRUE};
    inst::require(SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE,
        "backup_retention_delete_failed");
}
} // namespace

bool pre_setup_backup_contents(const archive::BackupInspection& inspection,
    const std::uint64_t current_generation) {
    if (!inspection.result.ok() || inspection.metadata.backup_type != "setup-upgrade") return false;
    const auto& generation = inspection.metadata.source_generation;
    std::uint64_t source{};
    const auto parsed = std::from_chars(generation.data(), generation.data()+generation.size(), source);
    if (parsed.ec != std::errc{} || parsed.ptr != generation.data()+generation.size() ||
        !source || source >= current_generation || generation != std::to_string(source)) return false;
    bool joker = false, other = false;
    for (const auto& file : inspection.files) {
        if (file.path == "Config/JOKER.DAT" && !file.size && !joker) { joker = true; continue; }
        if (file.path == "Config/otherbbs.txt" && !file.size && !other) { other = true; continue; }
        // Display content alone is not a configured BBS; the existing live
        // classifier likewise does not treat Display as initialized storage.
        // Archive path/duplicate/manifest validation is still mandatory.
        if (file.path.rfind("Display/", 0) == 0 && file.path.size() > 8) continue;
        return false; // includes DB, configuration, logs, FTN, and unknown roles
    }
    return joker && other;
}

bool verified_pre_setup_upgrade_backups(const fs::path& root, const std::wstring& owner,
    std::vector<inst::Handle>& leases) {
    try {
        const auto receipt = inst::read_receipt(root, owner);
        if (!receipt.committed) return false;
        auto collection = open_backup_collection(root, owner, archive::BackupKind::setup_upgrade, {}, false);
        auto listing = inst::open_object(collection.directory, true,
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE);
        std::set<fs::path> entries, accounted;
        for (const auto& entry : fs::directory_iterator(collection.directory)) {
            if (entries.size() >= 64) return false;
            entries.insert(entry.path());
        }
        if (entries.empty()) return false; // handled by the existing empty-child rule
        std::string identity;
        for (const auto ch : receipt.id) {
            if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'))) return false;
            identity.push_back(static_cast<char>(ch));
        }
        archive::Limits limits; limits.maximum_entries = 512;
        limits.maximum_file_bytes = 64ULL*1024*1024; limits.maximum_total_bytes = 64ULL*1024*1024;
        std::vector<inst::Handle> pins;
        std::set<std::uint64_t> sequences;
        for (const auto& path : entries) {
            if (path.extension() != L".zip") continue;
            const auto marker = fs::path(path.native()+L".sfx-owned");
            if (!entries.count(marker)) return false;
            // Bound compressed input before any hash/decompression. Retain the
            // same read lease while the existing native verifier authenticates it.
            auto sized = inst::open_object(path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(sized.get(), &size) || size.QuadPart < 1 || size.QuadPart > 16LL*1024*1024) return false;
            auto member = managed_member(path, owner, identity, archive::BackupKind::setup_upgrade, {}, &limits);
            if (!member || !pre_setup_backup_contents(member->inspection, receipt.generation) ||
                !sequences.insert(member->sequence).second) return false;
            accounted.insert(path); accounted.insert(marker);
            pins.push_back(std::move(sized));
            for (auto& pin : member->pins) pins.push_back(std::move(pin));
        }
        // No ignored orphan, directory, unowned ZIP, partial file, or unknown
        // history. Re-enumerate while every recognized member stays pinned.
        std::set<fs::path> current;
        for (const auto& entry : fs::directory_iterator(collection.directory)) {
            if (current.size() >= 64) return false;
            current.insert(entry.path());
        }
        if (accounted != entries || current != entries) return false;
        for (auto& pin : collection.anchors) leases.push_back(std::move(pin));
        leases.push_back(std::move(listing));
        for (auto& pin : pins) leases.push_back(std::move(pin));
        return true;
    } catch (const std::exception&) { return false; }
}

std::vector<VerifiedBackupMember> verified_backup_members(const fs::path& root,
    const std::wstring& owner,const BackupCollection& collection,archive::BackupKind kind,
    const archive::CancelCheck& cancelled){
    const auto receipt=inst::read_receipt(root,owner);inst::require(receipt.committed&&!collection.anchors.empty(),"file_backup_collection_unverified");
    std::string identity;for(const auto ch:receipt.id){inst::require(ch<=127,"file_backup_identity_invalid");identity+=static_cast<char>(ch);}
    std::vector<VerifiedBackupMember> result;std::size_t visited{};
    for(const auto& entry:fs::directory_iterator(collection.directory)){
        inst::require(++visited<=maximum_members&&(!cancelled||!cancelled()),"file_backup_inventory_limit_or_cancel");
        if(entry.path().extension()!=L".zip")continue;
        auto member=managed_member(entry.path(),owner,identity,kind,cancelled);if(!member)continue;
        result.push_back(take_verified(*member));
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
    for(std::size_t i=1;i<result.size();++i)inst::require(result[i-1].sequence!=result[i].sequence,"file_backup_sequence_ambiguous");
    return result;
}
std::vector<std::size_t> content_dependency_closure(const std::vector<VerifiedBackupMember>& members,const std::vector<std::size_t>& selected){
    std::set<std::size_t> retained;std::vector<std::size_t> pending=selected;
    while(!pending.empty()){
        const auto n=pending.back();pending.pop_back();inst::require(n<members.size(),"file_backup_restore_point_invalid");if(!retained.insert(n).second)continue;
        const auto& item=members[n];
        for(const auto& content:item.contents.files){if(content.archive.empty())continue;
            const auto found=std::find_if(members.begin(),members.end(),[&](const auto& m){return m.path.filename().u8string()==content.archive;});
            inst::require(found!=members.end()&&found->sequence<item.sequence&&found->sha256==content.archive_sha256,"file_backup_dependency_missing_or_changed");
            const auto payload=std::find_if(found->inspection.files.begin(),found->inspection.files.end(),[&](const auto& f){return f.path==content.member;});
            inst::require(payload!=found->inspection.files.end()&&payload->size==content.bytes&&payload->sha256==content.sha256,"file_backup_dependency_payload_mismatch");
            pending.push_back(static_cast<std::size_t>(found-members.begin()));
        }
    }
    return {retained.begin(),retained.end()};
}
void verify_content_dependencies(const std::vector<VerifiedBackupMember>& members,const std::vector<std::size_t>& selected){
    auto points=selected;if(points.empty())for(std::size_t n=0;n<members.size();++n)points.push_back(n);
    (void)content_dependency_closure(members,points);
}

BackupCollection open_backup_collection(const fs::path& root, const std::wstring& owner,
    const archive::BackupKind kind, const std::string& saved_destination, const bool create) {
    (void)pool_type(kind);
    inst::require(application::valid_backup_destination_text(saved_destination), "backup_destination_syntax_invalid");
    BackupCollection result;
    result.directory = kind == archive::BackupKind::setup_upgrade ? root / L"Backup" / L"Upgrades" :
        saved_destination.empty() ? root / L"Backup" / L"Scheduled" : fs::u8path(saved_destination);
    const auto text = result.directory.native();
    inst::require(application::valid_backup_destination_text(result.directory.u8string()) && text.size() <= 180,
        "backup_destination_path_invalid");
    // Do not permit a mutable-data/program/recovery folder to become an ordinary
    // destination. A private child of Backup is allowed and excluded from ZIPs.
    inst::require(!beneath(result.directory, root) ||
        (beneath(result.directory, root / L"Backup") && _wcsicmp(text.c_str(), (root / L"Backup").c_str())),
        "backup_destination_inside_live_role_refused");
    const auto drive = result.directory.root_path();
    wchar_t filesystem[32]{}; DWORD flags{};
    inst::require(GetDriveTypeW(drive.c_str()) == DRIVE_FIXED &&
        GetVolumeInformationW(drive.c_str(), nullptr, 0, nullptr, nullptr, &flags, filesystem, 32) &&
        !_wcsicmp(filesystem, L"NTFS") && (flags & FILE_PERSISTENT_ACLS), "backup_destination_requires_available_local_ntfs");
    std::array<wchar_t, MAX_PATH> windows{};
    inst::require(GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size())) != 0 &&
        !beneath(result.directory, fs::path(windows.data())), "backup_destination_system_tree_refused");
    auto path = drive;
    result.anchors.push_back(inst::open_object(path, true, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE));
    const auto relative = result.directory.relative_path();
    for (auto iterator = relative.begin(); iterator != relative.end(); ++iterator) {
        path /= *iterator;
        const bool final = std::next(iterator) == relative.end();
        if (final && missing(path)) {
            inst::require(create, "backup_destination_missing_no_fallback");
            auto descriptor = inst::mutable_descriptor(owner);
            inst::create_directory(path, descriptor.get());
        }
        result.anchors.push_back(inst::open_object(path, true, READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE));
    }
    inst::require(WindowsStorageDaclPolicy(owner).assess(result.directory,
        StorageDaclObjectKind::directory).status.succeeded(), "backup_destination_privacy_invalid_no_repair");
    return result;
}

std::size_t finish_backup_collection(const fs::path& root, const std::wstring& owner,
    const BackupCollection& collection, const fs::path& published, const archive::BackupKind kind,
    const std::uint32_t retained, archive::CancelCheck cancelled,
    std::function<void(std::size_t,std::size_t)> progress) {
    return finish_backup_collection(root,owner,collection,published,kind,retained,std::move(cancelled),std::move(progress),{});
}
std::size_t finish_backup_collection(const fs::path& root, const std::wstring& owner,
    const BackupCollection& collection, const fs::path& published, const archive::BackupKind kind,
    const std::uint32_t retained, archive::CancelCheck cancelled,
    std::function<void(std::size_t,std::size_t)> progress,
    std::function<void(BackupFinalizationStage)> stage) {
    inst::require(retained >= 1 && retained <= 365 && published.parent_path() == collection.directory &&
        !collection.anchors.empty(), "backup_retention_request_invalid");
    const auto receipt = inst::read_receipt(root, owner);
    inst::require(receipt.committed, "backup_retention_requires_committed_installation");
    std::string identity;
    for (const auto ch : receipt.id) {
        inst::require((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'), "backup_installation_identity_invalid");
        identity.push_back(static_cast<char>(ch));
    }
    std::vector<Member> members; std::uint64_t sequence{}; std::size_t inspected{};
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(collection.directory)) {
        inst::require(!cancelled || !cancelled(), "backup_retention_cancelled_preserved");
        inst::require(++inspected <= maximum_members, "backup_retention_inventory_limit_preserved");
        if (entry.path().extension() != L".zip" || entry.path() == published) continue;
        candidates.push_back(entry.path());
    }
    if(stage)stage(BackupFinalizationStage::existing_archives);
    std::size_t completed{};if(progress)progress(0,candidates.size());
    for(const auto& candidate:candidates){
        if (auto member = managed_member(candidate, owner, identity, kind, cancelled)) {
            sequence = std::max(sequence, member->sequence);
            members.push_back(std::move(*member));
        }
        if(progress)progress(++completed,candidates.size());
    }
    inst::require(sequence != std::numeric_limits<std::uint64_t>::max(), "backup_retention_sequence_exhausted");
    if(stage)stage(BackupFinalizationStage::published_archive);
    auto file = inst::open_object(published, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    const auto checked = archive::inspect_backup(published,{},cancelled);
    inst::require(checked.result.ok() && type_matches(checked.metadata.backup_type, kind),
        "backup_retention_publication_unverified");
    if(stage)stage(BackupFinalizationStage::ownership);
    const auto marker_path = fs::path(published.native() + L".sfx-owned");
    inst::require(WindowsStorageDaclPolicy(owner).assess(published, StorageDaclObjectKind::file).status.succeeded(),
        "backup_retention_publication_privacy_invalid");
    const auto digest = hash_handle(file.get(), cancelled);
    const std::string marker = "SFX-BACKUP-OWNER-1\n" + identity + "\n" + pool_type(kind) + "\n" +
        std::to_string(sequence + 1) + "\n" + checked.metadata.transaction_id + "\n" + digest + "\n" +
        published.filename().u8string() + "\n";
    auto descriptor = inst::mutable_descriptor(owner);
    inst::create_file(marker_path, {marker.begin(), marker.end()}, descriptor.get());
    inst::require(WindowsStorageDaclPolicy(owner).apply(marker_path,
        StorageDaclObjectKind::file).succeeded(), "backup_owner_marker_protection_failed");
    // New archive is retained even after a backwards wall-clock change.
    auto published_member=managed_member(published, owner, identity, kind, cancelled);
    inst::require(published_member.has_value(), "backup_owner_marker_verification_failed");
    file.reset();
    if(stage)stage(BackupFinalizationStage::dependencies);
    std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    // Ambiguous ordering cannot silently authorize cleanup.
    for (std::size_t i = 1; i < members.size(); ++i)
        inst::require(members[i - 1].sequence != members[i].sequence, "backup_retention_duplicate_sequence_preserved");
    // Verify the entire dependency closure BEFORE the first deletion. Keeping
    // a baseline as a dependency also preserves its own usable restore point.
    std::vector<VerifiedBackupMember> verified;
    for(auto& member:members)verified.push_back(take_verified(member));
    verified.push_back(take_verified(*published_member));
    std::sort(verified.begin(),verified.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
    std::vector<std::size_t> points;for(std::size_t i=verified.size()>retained?verified.size()-retained:0;i<verified.size();++i)points.push_back(i);
    const auto closure=content_dependency_closure(verified,points);std::set<fs::path> keep;
    for(const auto i:closure)keep.insert(verified[i].path);
    // Drop only deletion candidates' leases. Retained payload archives remain
    // pinned until pruning finishes; exact candidate IDs/hashes are rechecked.
    for(auto& item:verified)if(!keep.count(item.path))item.pins.clear();
    if(stage)stage(BackupFinalizationStage::retirement);
    std::size_t removed{};
    for(const auto& member:members) {
        if(keep.count(member.path))continue;
        const auto marker_file = fs::path(member.path.native() + L".sfx-owned");
        // Only this deletion candidate's leases were released above. The
        // DELETE handle below must still match its verified object ID, volume,
        // owner/DACL and full byte digest, including the provenance marker.
        delete_exact(member.path, member.file_id, member.volume, owner, member.digest, cancelled);
        delete_exact(marker_file, member.marker_id, member.volume, owner, member.marker_digest, cancelled);
        ++removed;
    }
    return removed;
}
} // namespace sfx::platform::win32
