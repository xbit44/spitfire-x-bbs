#include "sfx/platform/win32/clamav_installation.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/archive/archive_engine.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <algorithm>
#include <set>
#include <array>
#include <bcrypt.h>

namespace sfx::platform::win32 {
namespace {
namespace ins=installation;using Path=std::filesystem::path;using ins::require;
constexpr char previous_package_hash[]="AE0FF986504180B0966170FD058E1C337849174A015B2F7958479A04CDEE5F00";
constexpr wchar_t retained_engine_name[]=L"ClamAV-1.4.6-retained";
const auto& prefix=kClamAvPortableArchivePrefix;
bool absent(const Path& path){if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return false;const auto e=GetLastError();require(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND,"clamav_path_inspection");return true;}
std::vector<ins::Handle> pin_path(const Path& path,bool directory){
    require(ins::canonical_root(path)==path,"clamav_path_invalid");std::vector<Path> chain;
    for(auto p=directory?path:path.parent_path();!p.empty();){chain.push_back(p);const auto next=p.parent_path();if(next==p)break;p=next;}
    std::reverse(chain.begin(),chain.end());std::vector<ins::Handle> pins;
    for(const auto& p:chain)pins.push_back(ins::open_object(p,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE));
    if(!directory)pins.push_back(ins::open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ));return pins;
}
std::vector<std::uint8_t> receipt(const std::wstring& id,bool legacy=false){
    require(id.size()==32&&id.find_first_not_of(L"0123456789abcdefABCDEF")==std::wstring::npos,"clamav_installation_identity");
    std::string ascii_id;for(const auto ch:id)ascii_id.push_back(static_cast<char>(ch));
    const std::string version=legacy?"1.4.6":kClamAvPortableVersion;
    const std::string text="SFX-CLAMAV-1\n"+ascii_id+"\n"+version+"-win32\n"+
        (legacy?previous_package_hash:kClamAvPortableSha256)+"\n";
    return {text.begin(),text.end()};
}
bool matches_receipt(const std::vector<std::uint8_t>& bytes,const std::wstring& id,
    const std::wstring& predecessor,bool legacy){
    return bytes==receipt(id,legacy)||(!predecessor.empty()&&bytes==receipt(predecessor,legacy));
}
bool license_member(const std::string& name){
    if(name=="COPYING.txt"||name=="README.md"||name=="conf_examples/freshclam.conf.sample")return true;
    return name.rfind("COPYING/",0)==0&&name.size()>7&&name.find('/',7)==std::string::npos&&name.find_first_of("\\:")==std::string::npos&&name.substr(7)!="."&&name.substr(7)!="..";
}
void verify_package_held(const Path& zip){
    BY_HANDLE_FILE_INFORMATION info{};const auto held=ins::open_object(zip,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
    require(GetFileInformationByHandle(held.get(),&info)&&info.nNumberOfLinks==1&&!info.nFileSizeHigh&&info.nFileSizeLow==kClamAvPortableBytes,"clamav_package_size_or_links");
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    struct HashCleanup{BCRYPT_ALG_HANDLE& algorithm;BCRYPT_HASH_HANDLE& hash;~HashCleanup(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}} cleanup{algorithm,hash};
    require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0&&BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)==0,"clamav_package_hash_provider");
    std::array<unsigned char,65536> buffer{};std::uint64_t total{};DWORD got{};
    for(;;){require(ReadFile(held.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&got,nullptr),"clamav_package_read");if(!got)break;total+=got;require(total<=kClamAvPortableBytes&&BCryptHashData(hash,buffer.data(),got,0)==0,"clamav_package_hash");}
    std::array<unsigned char,32> digest{};require(total==kClamAvPortableBytes&&BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)==0,"clamav_package_hash");
    std::string text;for(const auto b:digest){text+="0123456789ABCDEF"[b>>4];text+="0123456789ABCDEF"[b&15];}
    require(text==kClamAvPortableSha256,"clamav_package_not_qualified_official_zip");
}
}
void verify_clamav_package(const Path& zip){const auto pins=pin_path(zip,false);verify_package_held(zip);}
bool clamav_installed(const Path& root){const auto tools=root/L"Bin"/L"Tools";return !absent(tools/L"ClamAV")||!absent(tools/retained_engine_name);}

namespace {
struct VerifiedEngine {
    VerifiedEngine(const Path& path, const std::wstring& owner)
        : directory(path),
          descriptor(ins::structural_descriptor(owner)), held(pin_path(directory, true)) {
        require(ins::exact_security(held.back().get(), descriptor.get()), "clamav_engine_protection");
        const auto marker = directory / L"SFX-component.receipt";
        identity = ins::open_object(marker, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(identity.get(), &info) && info.nNumberOfLinks == 1 &&
            ins::exact_security(identity.get(), descriptor.get()), "clamav_component_receipt");
        bytes = ins::read_file(marker, 1024);
    }

    void verify_members(const std::function<void()>& tick = {}, bool legacy = false) {
        const auto& members=legacy?legacy_clamav_program_members():clamav_program_members();
        for (const auto& member : members) {
            if (tick) tick();
            auto file = ins::open_object(directory / member.name, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
            BY_HANDLE_FILE_INFORMATION info{};
            require(GetFileInformationByHandle(file.get(), &info) && info.nNumberOfLinks == 1 &&
                !info.nFileSizeHigh && info.nFileSizeLow == member.bytes && ins::exact_security(file.get(), descriptor.get()) &&
                ins::file_sha256(directory / member.name) == member.sha256, "clamav_engine_member_verification");
            held.push_back(std::move(file));
        }
    }

    Path directory;
    ins::Descriptor descriptor;
    std::vector<ins::Handle> held;
    ins::Handle identity;
    std::vector<std::uint8_t> bytes;
};

Path generated_configuration_root(const std::string& text) {
    const std::string_view database_prefix = "DatabaseDirectory \"";
    const std::string_view suffix = "\\Data\\ClamAV\"\n";
    const auto end = text.find(suffix, database_prefix.size());
    require(text.compare(0, database_prefix.size(), database_prefix) == 0 && end != text.npos,
        "clamav_existing_configuration_changed");
    const auto spelling = text.substr(database_prefix.size(), end - database_prefix.size());
    require(spelling.size() >= 4 && spelling.size() <= 160 && spelling[0] >= 'A' && spelling[0] <= 'Z' &&
        spelling[1] == ':' && spelling[2] == '\\' && spelling.back() != '\\' &&
        spelling.find_first_of("/:*?\"<>|~#", 3) == spelling.npos && spelling.find("\\\\") == spelling.npos &&
        std::none_of(spelling.begin(), spelling.end(), [](unsigned char c) { return c < 32 || c == 127; }),
        "clamav_previous_configuration_path_invalid");
    // The previous volume can be absent. Treat its root strictly as text.
    const auto path = std::filesystem::u8path(spelling);
    require(path.lexically_normal() == path, "clamav_previous_configuration_path_invalid");
    return path;
}

bool rebase_clamav_configuration(const Path& root, const std::wstring& owner, const Path& source_root) {
    const auto path = root / L"Config" / L"ClamAV" / L"freshclam.conf";
    if (absent(path)) return false;
    const auto directory = FilePayloadDirectory::private_store(path.parent_path(), owner);
    auto file = ins::open_object(path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    const auto descriptor = ins::mutable_descriptor(owner);
    WindowsStorageDaclPolicy policy(owner);
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.get(), &info) && info.nNumberOfLinks == 1 &&
        (policy.assess(path, StorageDaclObjectKind::file).status.succeeded() || ins::exact_security(file.get(), descriptor.get())),
        "clamav_existing_configuration_security");
    const auto current = clamav_update_configuration(root);
    const auto raw = ins::read_file(path, 65536);
    const std::string text(raw.begin(), raw.end());
    if (text == current) return false;
    const auto old = source_root.empty() ? generated_configuration_root(text) : source_root;
    require(text == clamav_update_configuration(old), "clamav_existing_configuration_changed");
    const auto pending = path.parent_path() / (L"freshclam-" + ins::unique_id() + L".pending");
    ins::create_file(pending, {current.begin(), current.end()}, descriptor.get());
    if (!policy.apply(pending, StorageDaclObjectKind::file).succeeded()) {
        DeleteFileW(pending.c_str());
        require(false, "clamav_configuration_file_security");
    }
    file.reset();
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        require(false, "clamav_configuration_rebase_failed_preserved");
    }
    return true;
}
}

void verify_clamav_installation(const Path& root, const std::wstring& owner, const std::wstring& id) {
    VerifiedEngine engine(root/L"Bin"/L"Tools"/L"ClamAV", owner);
    require(engine.bytes == receipt(id), "clamav_component_receipt");
    engine.verify_members();
}

bool clamav_upgrade_required(const Path& root,const std::wstring& owner,const std::wstring& id,
    const std::wstring& predecessor) noexcept {
    if(!clamav_available())return false;
    try{
        const auto tools=root/L"Bin"/L"Tools";
        const auto active=tools/L"ClamAV";
        VerifiedEngine engine(absent(active)?tools/retained_engine_name:active,owner);
        return matches_receipt(engine.bytes,id,predecessor,true);
    }catch(...){return false;}
}

ClamAvRebindResult rebind_clamav_receipt(const Path& root, const std::wstring& owner, const std::wstring& id,
    const std::wstring& predecessor, const Path& source_root, const std::function<void()>& tick) {
    require(clamav_available() && ins::elevated(), "clamav_rebind_requires_consented_setup_or_prepare_worker");
    VerifiedEngine engine(root/L"Bin"/L"Tools"/L"ClamAV", owner);
    const bool legacy=matches_receipt(engine.bytes,id,predecessor,true);
    const auto current = receipt(id,legacy);
    const bool receipt_changed = engine.bytes != current;
    require(!receipt_changed || (!predecessor.empty()&&engine.bytes == receipt(predecessor,legacy)), "clamav_component_receipt");
    engine.verify_members(tick,legacy);
    ClamAvRebindResult result;
    try {
        // Finish configuration first so retrying a stale receipt can also
        // finish a previous interrupted configuration publication.
        result.configuration_changed = rebase_clamav_configuration(root, owner, source_root);
    } catch (const std::exception&) {
        result.configuration_preserved = true;
    }
    if (receipt_changed) {
        const auto marker = engine.directory / L"SFX-component.receipt";
        const auto pending = engine.directory / (L"SFX-component-" + ins::unique_id() + L".pending");
        ins::create_file(pending, current, engine.descriptor.get());
        // Prepare and Setup hold the offline guard and the protected engine path.
        // Keep the engine members pinned; only the marker must be released to rename.
        engine.identity.reset();
        if (!MoveFileExW(pending.c_str(), marker.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(pending.c_str());
            require(false, "clamav_component_receipt_rebind_failed_preserved");
        }
        result.receipt_changed = true;
    }
    return result;
}
void provision_clamav_engine(const Path& root,const std::wstring& owner,const std::wstring& id,const Path& zip,
    const std::function<void()>& tick,const std::wstring& predecessor){
    require(clamav_available(),"optional_component_unavailable_on_this_os");
    require(ins::elevated(),"clamav_provision_requires_setup_worker");const auto source=pin_path(zip,false);verify_package_held(zip);
    const auto tools=root/L"Bin"/L"Tools";const auto pins=pin_path(tools,true);const auto descriptor=ins::structural_descriptor(owner);
    require(ins::exact_security(pins.back().get(),descriptor.get()),"clamav_tools_protection");
    const auto active=tools/L"ClamAV",previous=tools/retained_engine_name;
    const bool active_exists=!absent(active),previous_exists=!absent(previous);
    if(active_exists||previous_exists){
        VerifiedEngine existing(active_exists?active:previous,owner);
        if(active_exists&&existing.bytes==receipt(id)){existing.verify_members(tick);return;}
        require(matches_receipt(existing.bytes,id,predecessor,true),"clamav_component_receipt");
        require(!active_exists||!previous_exists,"clamav_upgrade_retained_engine_conflict");
        existing.verify_members(tick,true);
    }
    archive::Limits limits;limits.maximum_entries=10000;limits.maximum_file_bytes=256ULL*1024*1024;limits.maximum_total_bytes=2ULL*1024*1024*1024;
    const auto inspection=archive::inspect_zip(zip,limits);require(inspection.result.ok(),"clamav_zip_invalid");
    std::vector<archive::FileRecord> selected;std::set<std::string> names;std::size_t images{},licenses{};
    for(const auto& f:inspection.files){require(f.path.rfind(prefix,0)==0,"clamav_package_root");const auto relative=f.path.substr(std::char_traits<char>::length(prefix));
        const auto image=std::find_if(clamav_program_members().begin(),clamav_program_members().end(),[&](const auto& m){return relative==Path(m.name).u8string();});
        if(image!=clamav_program_members().end()){require(f.size==image->bytes&&f.sha256==image->sha256,"clamav_zip_member_pin");++images;}
        else if(license_member(relative)){require(f.size<=4U*1024U*1024U,"clamav_license_bound");++licenses;}else continue;
        require(names.insert(relative).second,"clamav_duplicate_member");selected.push_back({relative,f.size,f.sha256});
    }
    require(images==clamav_program_members().size()&&licenses>2&&names.count("COPYING.txt")&&names.count("README.md"),"clamav_package_members_missing");
    const auto pending=tools/(L"ClamAV-pending-"+ins::unique_id());ins::create_directory(pending,descriptor.get());
    ins::create_directory(pending/L"COPYING",descriptor.get());ins::create_directory(pending/L"conf_examples",descriptor.get());
    for(const auto& f:selected){if(tick)tick();const auto content=archive::read_zip_member(zip,std::string(prefix)+f.path,32U*1024U*1024U);
        require(content.result.ok()&&content.bytes.size()==f.size&&ins::sha256(content.bytes)==f.sha256,"clamav_member_changed");
        ins::create_file(pending/std::filesystem::u8path(f.path),content.bytes,descriptor.get());}
    ins::create_file(pending/L"SFX-component.receipt",receipt(id),descriptor.get());
    {
        VerifiedEngine staged(pending,owner);
        require(staged.bytes==receipt(id),"clamav_staged_receipt");staged.verify_members(tick);
    }
    if(active_exists)require(MoveFileExW(active.c_str(),previous.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,
        "clamav_upgrade_original_engine_preserved");
    if(!MoveFileExW(pending.c_str(),active.c_str(),MOVEFILE_WRITE_THROUGH)){
        if(active_exists)require(MoveFileExW(previous.c_str(),active.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,
            "clamav_upgrade_interrupted_previous_engine_retained");
        require(false,"clamav_engine_publication_failed_pending_preserved");
    }
}
std::string clamav_update_configuration(const Path& root){
    // The pinned Windows FreshClam filesystem path parser requires native
    // backslashes, including inside quoted paths containing spaces.
    const auto path=[&](const wchar_t* role){auto native=root/role;native.make_preferred();auto s=native.u8string();require(!s.empty()&&s.find_first_of("\r\n\"#")==std::string::npos,"clamav_configuration_path");return "\""+s+"\"";};
    return "DatabaseDirectory "+path(L"Data/ClamAV")+"\nUpdateLogFile "+path(L"Logs/ClamAV/freshclam.log")+
        "\nDatabaseMirror database.clamav.net\nMaxAttempts 1\nConnectTimeout 15\nReceiveTimeout 30\nLogTime yes\nLogFileMaxSize 2M\nLogRotate yes\nTestDatabases yes\nForeground yes\n";
}
void initialize_clamav_data(const Path& root,const std::wstring& owner){
    require(clamav_available(),"optional_component_unavailable_on_this_os");
    require(ins::is_owned_service_runtime(root,owner)||(!ins::elevated()&&owner==ins::operator_sid()),"clamav_data_requires_authorized_owner");const auto descriptor=ins::mutable_descriptor(owner);
    std::vector<FilePayloadDirectory> parents;
    for(const auto* role:{L"Config",L"Data",L"Logs",L"Work"}){
        parents.push_back(FilePayloadDirectory::private_store(root/role,owner));const auto path=root/role/L"ClamAV";
        if(absent(path))ins::create_directory(path,descriptor.get());parents.push_back(FilePayloadDirectory::private_store(path,owner));}
    const auto configuration=clamav_update_configuration(root);const auto file=root/L"Config"/L"ClamAV"/L"freshclam.conf";
    const std::vector<std::uint8_t> expected(configuration.begin(),configuration.end());
    auto directory=FilePayloadDirectory::private_store(file.parent_path(),owner);
    if(absent(file)){auto created=directory.create("freshclam.conf",expected.size());created.append(expected.data(),expected.size());(void)created.identity();}
    else {
        // Repair only the exact generated configuration left by the pre-release
        // directory-descriptor bug. No changed bytes, foreign ACL, linked file,
        // parent permission or user configuration is adopted or overwritten.
        auto held=ins::open_object(file,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(held.get(),&info)&&info.nNumberOfLinks==1&&ins::read_file(file,65536)==expected,"clamav_existing_configuration_changed");
        WindowsStorageDaclPolicy policy(owner);
        if(!policy.assess(file,StorageDaclObjectKind::file).status){
            const auto installed=ins::read_receipt(root,owner);verify_clamav_installation(root,owner,installed.id);
            require(ins::exact_security(held.get(),descriptor.get()),"clamav_existing_configuration_security");
            require(policy.apply(file,StorageDaclObjectKind::file).succeeded(),"clamav_configuration_file_security");
        }
    }
    auto held=directory.read("freshclam.conf");
    require(held.identity().bytes==expected.size()&&held.read(0,expected.size()+1)==expected,"clamav_existing_configuration_changed");
}
bool clamav_bootstrap_metadata(const Path& root,const Path& candidate,const std::wstring& owner)noexcept{
    try{
        const bool config=candidate==root/L"Config"/L"ClamAV";
        const bool data=candidate==root/L"Data"/L"ClamAV";
        const bool logs=candidate==root/L"Logs"/L"ClamAV";
        if(!config&&!data&&!logs)return false;
        // Classification proves private Setup metadata, not executable trust.
        // Scan/update use verifies every engine member independently.
        const auto installed=ins::read_receipt(root,owner);
        const auto engine=root/L"Bin"/L"Tools"/L"ClamAV";
        const auto pins=pin_path(engine,true);const auto structural=ins::structural_descriptor(owner);
        require(ins::exact_security(pins.back().get(),structural.get()),"clamav_engine_protection");
        const auto marker=ins::open_object(engine/L"SFX-component.receipt",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION marker_info{};
        require(GetFileInformationByHandle(marker.get(),&marker_info)&&marker_info.nNumberOfLinks==1&&
            ins::exact_security(marker.get(),structural.get()),"clamav_component_receipt");
        const auto marker_bytes=ins::read_file(engine/L"SFX-component.receipt",1024);
        require(marker_bytes==receipt(installed.id)||marker_bytes==receipt(installed.id,true),"clamav_component_receipt");
        auto directory=FilePayloadDirectory::private_store(candidate,owner);
        WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((candidate/L"*").c_str(),&entry);
        require(find!=INVALID_HANDLE_VALUE,"clamav_metadata_enumeration");struct Close{HANDLE h;~Close(){FindClose(h);}} close{find};
        std::vector<ins::Handle> files;bool configuration_seen{};std::uint64_t total{};
        do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
            require(!(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&files.size()<16,"clamav_metadata_unknown_child");
            const bool known=config?(name==L"freshclam.conf"||name==L"update-state.dat"||name==L"SFX-update-status.dat"):logs?(name==L"freshclam.log"||name==L"freshclam.log.1"):
                (name==L"main.cvd"||name==L"main.cld"||name==L"daily.cvd"||name==L"daily.cld"||name==L"bytecode.cvd"||name==L"bytecode.cld"||name==L"freshclam.dat");
            require(known,"clamav_metadata_unknown_child");const auto path=candidate/name;
            auto held=ins::open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);BY_HANDLE_FILE_INFORMATION info{};
            require(GetFileInformationByHandle(held.get(),&info)&&info.nNumberOfLinks==1&&!info.nFileSizeHigh,"clamav_metadata_file_identity");
            total+=info.nFileSizeLow;require(total<=(data?2ULL*1024*1024*1024:logs?4ULL*1024*1024:65536),"clamav_metadata_size");
            const bool state=config&&name==L"update-state.dat";
            require(state?ins::exact_security(held.get(),ins::mutable_descriptor(owner).get()):WindowsStorageDaclPolicy(owner).assess(path,StorageDaclObjectKind::file,true).status.succeeded(),"clamav_metadata_private_policy");
            if(config&&name==L"freshclam.conf"){const auto expected=clamav_update_configuration(root);require(ins::read_file(path,65536)==std::vector<std::uint8_t>(expected.begin(),expected.end()),"clamav_metadata_configuration");configuration_seen=true;}
            if(state){const auto bytes=ins::read_file(path,80);const std::string text(bytes.begin(),bytes.end()),state_prefix="SFX-CLAMAV-UPDATE-1\n";require(text.rfind(state_prefix,0)==0&&text.size()>state_prefix.size()+1&&text.back()=='\n'&&text.substr(state_prefix.size(),text.size()-state_prefix.size()-1).find_first_not_of("0123456789")==std::string::npos,"clamav_metadata_update_state");}
            if(config&&name==L"SFX-update-status.dat")require(valid_clamav_update_status(ins::read_file(path,1024)),"clamav_metadata_update_history");
            files.push_back(std::move(held));
        }while(FindNextFileW(find,&entry));require(GetLastError()==ERROR_NO_MORE_FILES&&(!config||configuration_seen),"clamav_metadata_incomplete");return true;
    }catch(...){return false;}
}
#include "clamav_retirement.inc"
} // namespace sfx::platform::win32
