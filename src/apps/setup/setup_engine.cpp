#include "setup_engine.hpp"
#include <chrono>
#include "sfx/platform/win32/backup_read_access.hpp"
#include "stock_menu_rc10.hpp"
#include "maintenance_progress.hpp"
#include "clamav_download.hpp"
#include "../product_version.h"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/other_board_text_file.hpp"
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/platform/win32/file_backup.hpp"
#include "sfx/platform/win32/door_component.hpp"
#include "sfx/platform/win32/starter_doors.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/service_secret_bridge.hpp"
#include <chrono>
#include <set>
#include <shellapi.h>
#include <shlobj.h>
#include <propsys.h>
#include <initguid.h>
#include <propkey.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace sfx::apps::setup {
namespace {
using namespace install;
namespace fs = std::filesystem;
namespace archive_engine = sfx::archive;
constexpr std::size_t maximum_program_bytes = 128U * 1024U * 1024U;
constexpr DWORD maximum_packet = 4096;
constexpr DWORD pipe_timeout_ms = 60000;
constexpr DWORD installation_timeout_ms = 3600000;
#include "stock_menu_payload.inc"
#include "starter_door_payload.inc"
#include "sysop_docs_payload.inc"

std::string ascii(const std::wstring& text) {
    require(std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= 32 && c < 127; }),
        "setup_non_ascii_control_value");
    std::string result; for (auto c : text) result += static_cast<char>(c); return result;
}
std::wstring wide(const std::string& text) { return {text.begin(), text.end()}; }
bool introducing_botgate_art(const Plan& plan) {
    if(!plan.previous) return false;
    const auto executable=plan.root/L"Bin"/programs[runtime_program_index];
    auto held=open_object(executable,false,GENERIC_READ,FILE_SHARE_READ);
    require(sha256(read_file(executable,maximum_program_bytes))==plan.previous->program_hashes[runtime_program_index],"setup_old_program_changed");
    DWORD unused{};const auto size=GetFileVersionInfoSizeW(executable.c_str(),&unused);
    require(size && size<=1024*1024,"setup_old_version_unavailable");
    std::vector<std::uint8_t> bytes(size);
    require(GetFileVersionInfoW(executable.c_str(),0,size,bytes.data())!=FALSE,"setup_old_version_unavailable");
    VS_FIXEDFILEINFO* version{};UINT length{};
    require(VerQueryValueW(bytes.data(),L"\\",reinterpret_cast<void**>(&version),&length) && length>=sizeof(*version) && version->dwSignature==0xfeef04bd,"setup_old_version_invalid");
    return version->dwFileVersionMS==0 && version->dwFileVersionLS<0x00030000;
}
std::vector<std::uint8_t> resource(int id) {
    const auto module = GetModuleHandleW(nullptr);
    auto found = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    require(found != nullptr, "setup_payload_missing");
    auto loaded = LoadResource(module, found); auto* data = static_cast<const BYTE*>(LockResource(loaded));
    const auto size = SizeofResource(module, found);
    require(data && size && size <= maximum_program_bytes, "setup_payload_size_invalid");
    return {data, data + size};
}
std::array<std::vector<std::uint8_t>, current_program_count> payload() {
    std::array<std::vector<std::uint8_t>, current_program_count> result{
        resource(201), resource(202), resource(204), resource(205), resource(206), resource(228),resource(234),resource(235),resource(236),resource(237),resource(238),resource(239)};
    const auto raw = resource(203); const std::string manifest(raw.begin(), raw.end());
    std::string expected="SFX-PAYLOAD-8\n";for(std::size_t n=0;n<result.size();++n)expected+=(n==dos_launcher_program_index?"MZ16 ":"PE32 ")+sha256(result[n])+"\n";
    expected+="PE32-DLL "+sha256(resource(240))+"\nINI-PRESERVE "+sha256(resource(241))+"\n";
    require(manifest==expected,"setup_payload_hash_mismatch");
    require(sha256(resource(240))==qualified_door_driver_sha256,"setup_dos_driver_not_exact");
    require(sha256(result[dos_launcher_program_index])=="7512CC632275C310C29E3C8FF5554270C4C7885CF637A1528CCC8A47A101FBA2","setup_dos_launcher_not_exact");
    require(sha256(result[5])=="F6C7E6079AA68F3C10A902A8CC90257C8F0BA158FCEEC76EDA0C9E2CBB7DFB91","setup_message_helper_not_approved");
    require(sha256(result[6])=="D813790C72BA1EDDA5F2E920719798E05A0DF4D722C2E557A32A1678C800F998","setup_packet_helper_not_exact");
    require(sha256(result[file_transfer_program_index])=="8B87D172E85FE23AA792A984E69DC305292A74D125F7414B7607218B487479A4","setup_general_file_helper_not_exact");
    for (std::size_t index = 0; index != result.size(); ++index) {
        const auto& program = result[index];
        require(program.size() > sizeof(IMAGE_DOS_HEADER), "setup_program_header_missing");
        IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, program.data(), sizeof(dos));
        if(index==dos_launcher_program_index){
            const auto extent=dos.e_cp?static_cast<std::size_t>(dos.e_cp-1)*512+(dos.e_cblp?dos.e_cblp:512):0;
            require(dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_cp&&dos.e_cblp<512&&extent==program.size()&&dos.e_cparhdr>=2&&static_cast<std::size_t>(dos.e_cparhdr)*16<extent&&static_cast<std::size_t>(dos.e_lfarlc)+static_cast<std::size_t>(dos.e_crlc)*4<=static_cast<std::size_t>(dos.e_cparhdr)*16,"setup_dos_mz_invalid");
            continue;
        }
        require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
            static_cast<std::size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS32) <= program.size(),
            "setup_program_header_invalid");
        IMAGE_NT_HEADERS32 nt{}; std::memcpy(&nt, program.data() + dos.e_lfanew, sizeof(nt));
        require(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
            nt.OptionalHeader.Subsystem ==
                ((index == control_panel_program_index ||
                  index == runtime_program_index ||
                  index == configuration_program_index || index == file_manager_program_index || index == maintenance_program_index)
                    ? IMAGE_SUBSYSTEM_WINDOWS_GUI
                    : IMAGE_SUBSYSTEM_WINDOWS_CUI) &&
            nt.OptionalHeader.MajorSubsystemVersion <= 6 &&
            (nt.OptionalHeader.MajorSubsystemVersion != 6 || nt.OptionalHeader.MinorSubsystemVersion <= 1),
            "setup_program_architecture_invalid");
    }
    return result;
}
bool absent(const Path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "setup_target_inspection_failed");
    return true;
}
void carry_portable_dos_input(const Path& root,const std::wstring& owner){
    const auto bytes=resource(240);require(sha256(bytes)==qualified_door_driver_sha256,"setup_portable_dos_input_not_exact");
    const auto directory=root/L"Bin"/L"Support";const auto path=directory/L"SFXEXEC.DLL";auto descriptor=structural_descriptor(owner);
    if(absent(directory))create_directory(directory,descriptor.get());else{auto pin=open_object(directory,true);(void)pin;}
    if(!absent(path)){auto pin=open_object(path,false,GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);require(file_sha256(path)==qualified_door_driver_sha256,"setup_portable_dos_input_conflict_preserved");return;}
    create_file(path,bytes,descriptor.get());require(file_sha256(path)==qualified_door_driver_sha256,"setup_portable_dos_input_publication_unconfirmed");
}
void seed_door_ini(const Path& root,const std::wstring& owner){
    const auto path=root/L"Bin"/L"sfxexec.ini";
    if(!absent(path)){auto existing=open_object(path,false);(void)existing;return;}
    const auto ini=resource(241);require(ini.size()<=16384,"setup_door_ini_oversized");auto sd=mutable_descriptor(owner);create_file(path,ini,sd.get());
}
std::optional<bool> component_generation_committed(const Receipt& expected,bool remove){
    try{
        HKEY raw{};const auto name=L"SOFTWARE\\Spitfire-X BBS.Installation."+expected.id;
        const auto status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,name.c_str(),REG_OPTION_OPEN_LINK,KEY_READ|KEY_WOW64_64KEY,&raw);
        if(raw)RegCloseKey(raw);
        if(status==ERROR_FILE_NOT_FOUND)return remove;
        if(status!=ERROR_SUCCESS)return {};
        if(remove){
            const auto records=registered_installations({expected.id},true);
            if(records.size()==1&&encode_receipt(records.front().receipt)==encode_receipt(expected))return false;
        }
        auto root=open_object(expected.root,true);if(file_id(root.get())!=expected.root_id||volume_serial(root.get())!=expected.volume)return {};
        const auto actual=read_receipt(expected.root,expected.owner);
        if(remove)return false;
        return encode_receipt(actual)==encode_receipt(expected);
    }catch(...){return {};}
}
DoorComponentPolicy door_component_policy(){
    // There is no shipped older incompatible core in this lineage. Keep the
    // predecessor allow-list empty until such a pair is explicitly reviewed.
    return {qualified_door_driver_sha256,{},[](const Receipt& consumer){try{
        const auto actual=read_receipt(consumer.root,consumer.owner);if(encode_receipt(actual)!=encode_receipt(consumer))return false;
        verify_structure(actual);std::vector<Handle> held;
        for(std::size_t n=0;n<actual.program_count;++n)held.push_back(open_object(actual.root/L"Bin"/programs[n],false,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ));
        return true;
    }catch(...){return false;}}};
}

void refresh_known_stock_menu_pairs(const Path&,const std::wstring&,const Progress&);
void seed_missing_stock_menus(const Path& root, const std::wstring& owner,const Progress& progress) {
    auto directory = open_object(root/L"Display",true);
    auto descriptor = mutable_descriptor(owner);
    platform::win32::WindowsStorageDaclPolicy policy(owner);
    std::set<std::wstring> preserve_pair;
    for(const auto& pair:rc10_stock_pairs){const std::wstring stem=pair.stem;
        if(!absent(root/L"Display"/(stem+L".ans"))||!absent(root/L"Display"/(stem+L".asc"))){preserve_pair.insert(stem+L".ans");preserve_pair.insert(stem+L".asc");}}
    for (const auto& asset : stock_menu_assets) {
        if(preserve_pair.count(asset.filename))continue;
        const auto file = root/L"Display"/asset.filename;
        if (!absent(file)) continue; // independent per filename; never replace
        const auto bytes = resource(asset.resource_id);
        require(sha256(bytes)==asset.sha256,"setup_stock_menu_payload_hash_mismatch");
        create_file(file,bytes,descriptor.get()); // CREATE_NEW; no race overwrite
        require(policy.apply(file,platform::win32::StorageDaclObjectKind::file).succeeded(),
            "setup_stock_menu_protection_failed");
        require(sha256(read_file(file,256U*1024U))==asset.sha256,"setup_stock_menu_installed_hash_mismatch");
    }
    refresh_known_stock_menu_pairs(root,owner,progress);
}
// The receipt records only files actually created or recognized as unmodified
// shipped content. Unknown/custom files are never adopted by matching new media.
std::optional<std::vector<std::uint8_t>> read_doc_handle(HANDLE file){
    LARGE_INTEGER size{},zero{};require(GetFileSizeEx(file,&size)&&size.QuadPart>=0,"setup_doc_size_invalid");
    if(size.QuadPart>1024*1024)return {}; // Owner content is preserved, never read unbounded.
    require(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE,"setup_doc_seek_failed");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));DWORD count{};
    require(bytes.empty()||(ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()),"setup_doc_read_failed");return bytes;
}
bool update_stock_doc(const Path& root,const Path& path,const std::vector<std::uint8_t>& bytes,
    const std::string& prior,PSECURITY_DESCRIPTOR descriptor){
    if(absent(path)){create_file(path,bytes,descriptor);return true;}
    if(prior.empty())return false;
    {
        auto probe=open_object(path,false,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
        const auto current=read_doc_handle(probe.get());if(!current||sha256(*current)!=prior)return false;
        if(*current==bytes)return true; // An unchanged doc needs no write lease.
    }
    auto handle=open_object(path,false,GENERIC_READ|GENERIC_WRITE,0);
    const auto read=read_doc_handle(handle.get());if(!read)return false;const auto& original=*read;if(sha256(original)!=prior)return false;
    if(original==bytes)return true;
    // Exclusive access keeps an owner's concurrent write/rename out of the
    // compare-and-update window. A failed update leaves this exact recovery copy.
    const auto recovery=root/L"Config"/(L".sfx-doc-recovery-"+unique_id()+L".txt");
    create_file(recovery,original,descriptor);
    LARGE_INTEGER zero{};DWORD count{};require(SetFilePointerEx(handle.get(),zero,nullptr,FILE_BEGIN)!=FALSE,"setup_doc_seek_failed");
    require((bytes.empty()||(WriteFile(handle.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()))&&SetEndOfFile(handle.get())&&FlushFileBuffers(handle.get()),"setup_doc_update_failed_recovery_preserved");
    require(read_doc_handle(handle.get())==std::optional<std::vector<std::uint8_t>>(bytes),"setup_doc_update_unconfirmed_recovery_preserved");
    require(read_file(recovery,1024U*1024U)==original&&DeleteFileW(recovery.c_str()),"setup_doc_recovery_cleanup_unconfirmed");
    return true;
}
void refresh_known_stock_menu_pairs(const Path& root,const std::wstring& owner,const Progress& progress){
    auto descriptor=mutable_descriptor(owner);
    for(const auto& pair:rc10_stock_pairs){
        const std::wstring stem=pair.stem;const std::array<Path,2> paths{root/L"Display"/(stem+L".ans"),root/L"Display"/(stem+L".asc")};
        if(absent(paths[0])||absent(paths[1])){if(progress)progress(L"Keeping incomplete menu pair: "+stem+L".ans / .asc.");continue;}
        std::array<Handle,2> handles;std::array<std::vector<std::uint8_t>,2> original,replacement;
        bool matched=true;
        for(std::size_t n=0;n<2;++n){
            handles[n]=open_object(paths[n],false,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
            const auto read=read_doc_handle(handles[n].get());
            if(!read||sha256(*read)!=(n?pair.plain:pair.ansi)){matched=false;break;}
            original[n]=*read;
            const auto asset=std::find_if(stock_menu_assets.begin(),stock_menu_assets.end(),[&](const auto& value){return paths[n].filename()==value.filename;});
            require(asset!=stock_menu_assets.end(),"setup_stock_pair_payload_missing");replacement[n]=resource(asset->resource_id);
            require(sha256(replacement[n])==asset->sha256,"setup_stock_pair_payload_changed");
        }
        if(!matched){if(progress)progress(L"Keeping current or customized menu pair: "+stem+L".ans / .asc.");continue;}
        for(auto& handle:handles)handle.reset();
        for(std::size_t n=0;n<2;++n){
            handles[n]=open_object(paths[n],false,GENERIC_READ|GENERIC_WRITE,0);
            require(read_doc_handle(handles[n].get())==std::optional<std::vector<std::uint8_t>>(original[n]),"setup_stock_pair_changed_preserved");
        }
        const auto token=unique_id();const std::array<Path,2> backup{root/L"Display"/(L".sfx-menu-"+token+L".ans"),root/L"Display"/(L".sfx-menu-"+token+L".asc")};
        for(std::size_t n=0;n<2;++n)create_file(backup[n],original[n],descriptor.get());
        const auto write=[&](std::size_t n,const std::vector<std::uint8_t>& bytes){LARGE_INTEGER zero{};DWORD count{};
            require(SetFilePointerEx(handles[n].get(),zero,nullptr,FILE_BEGIN)&&WriteFile(handles[n].get(),bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()&&SetEndOfFile(handles[n].get())&&FlushFileBuffers(handles[n].get()),"setup_stock_pair_write_failed_recovery_retained");
            require(read_doc_handle(handles[n].get())==std::optional<std::vector<std::uint8_t>>(bytes),"setup_stock_pair_verify_failed_recovery_retained");};
        try{for(std::size_t n=0;n<2;++n)write(n,replacement[n]);}
        catch(...){for(std::size_t n=0;n<2;++n){try{write(n,original[n]);}catch(...){}}throw;}
        for(std::size_t n=0;n<2;++n)require(read_file(backup[n],256U*1024U)==original[n]&&DeleteFileW(backup[n].c_str()),"setup_stock_pair_recovery_cleanup_unconfirmed");
        if(progress)progress(L"Updated matching stock menu pair: "+stem+L".ans / .asc.");
    }
}
void seed_missing_sysop_docs(const Path& root,const std::wstring& owner){
    if(sysop_doc_assets.empty())return;
    auto descriptor=mutable_descriptor(owner);std::vector<Handle> held;
    for(const auto* relative:{L"Config",L"Docs",L"Docs/Sysop",L"Docs/Sysop/assets",L"Docs/Sysop/images",L"Docs/Doors",L"Docs/ThirdParty"}){
        const auto path=(root/relative).make_preferred();if(absent(path))create_directory(path,descriptor.get());held.push_back(open_object(path,true));
    }
    const auto receipt=root/L"Config"/L"stock-docs.v1",legacy=root/L"Docs"/L".sfx-stock-docs.txt";
    std::vector<std::uint8_t> previous,legacy_bytes;std::map<std::string,std::string> known;
    const auto parse=[](const std::vector<std::uint8_t>& bytes,std::map<std::string,std::string>& into){
        if(bytes.size()>16384)return false;const std::string text(bytes.begin(),bytes.end());
        if(text.rfind("SFX-DOCS-1\n",0)!=0)return false;
        std::map<std::string,std::string> parsed;std::size_t at=11;
        while(at<text.size()){
            const auto end=text.find('\n',at),bar=text.find('|',at);if(end==std::string::npos||bar==std::string::npos||bar>=end)return false;
            const auto hash=text.substr(bar+1,end-bar-1);if(hash.size()!=64||hash.find_first_not_of("0123456789ABCDEF")!=std::string::npos)return false;
            if(!parsed.emplace(text.substr(at,bar-at),hash).second)return false;at=end+1;
        }
        into=std::move(parsed);return true;
    };
    if(!absent(receipt)){
        auto handle=open_object(receipt,false,GENERIC_READ,FILE_SHARE_READ);const auto read=read_doc_handle(handle.get());
        if(!read||!parse(*read,known))return;previous=*read;
    }
    bool recognized_legacy=false;
    if(!absent(legacy)){
        auto handle=open_object(legacy,false,GENERIC_READ,FILE_SHARE_READ);const auto read=read_doc_handle(handle.get());
        std::map<std::string,std::string> old;
        if(read&&parse(*read,old)){
            legacy_bytes=*read;recognized_legacy=true;
            // The internal receipt wins after an interrupted migration; every
            // earlier hash remains in the exact retained legacy receipt below.
            for(const auto& row:old)known.emplace(row);
        }else if(previous.empty())return; // Unknown owner metadata is untouched.
    }
    for(const auto& asset:sysop_doc_assets){
        const auto path=(root/asset.filename).make_preferred();const auto key=ascii(asset.filename);const auto prior=known.find(key);
        Path parent=root;for(const auto& part:Path(asset.filename).parent_path()){
            parent/=part;if(absent(parent))create_directory(parent,descriptor.get());held.push_back(open_object(parent,true));
        }
        if(!absent(path)){
            if(prior==known.end()&&key.rfind("Docs/Doors/",0)==0){
                auto handle=open_object(path,false,GENERIC_READ,FILE_SHARE_READ);const auto read=read_doc_handle(handle.get());
                if(read&&sha256(*read)==asset.sha256)known[key]=asset.sha256;
                continue;
            }
            if(prior==known.end())continue;auto handle=open_object(path,false,GENERIC_READ,FILE_SHARE_READ);const auto read=read_doc_handle(handle.get());if(!read||sha256(*read)!=prior->second)continue;
        }
        const auto bytes=resource(asset.resource_id);require(sha256(bytes)==asset.sha256,"setup_sysop_doc_payload_mismatch");
        if(update_stock_doc(root,path,bytes,prior==known.end()?std::string{}:prior->second,descriptor.get()))known[key]=asset.sha256;
    }
    std::string next="SFX-DOCS-1\n";for(const auto& row:known)next+=row.first+"|"+row.second+"\n";
    const std::vector<std::uint8_t> bytes(next.begin(),next.end());
    require(update_stock_doc(root,receipt,bytes,previous.empty()?std::string{}:sha256(previous),descriptor.get()),"setup_doc_receipt_changed_preserved");
    if(recognized_legacy){
        const auto hash=sha256(legacy_bytes);const std::wstring wide(hash.begin(),hash.end());
        const auto retained=root/L"Config"/(L"stock-docs-legacy-"+wide+L".v1");
        if(absent(retained))create_file(retained,legacy_bytes,descriptor.get());
        require(read_file(retained,16384)==legacy_bytes,"setup_legacy_doc_receipt_preservation_failed");
        auto handle=open_object(legacy,false,GENERIC_READ|DELETE,0);
        require(read_doc_handle(handle.get())==std::optional<std::vector<std::uint8_t>>(legacy_bytes),"setup_legacy_doc_receipt_changed_preserved");
        FILE_DISPOSITION_INFO remove{TRUE};require(SetFileInformationByHandle(handle.get(),FileDispositionInfo,&remove,sizeof(remove)),"setup_legacy_doc_receipt_retirement_failed");
    }
}
void ensure_file_store_directories(const Path& root,const std::wstring& owner){
    const auto descriptor=mutable_descriptor(owner);platform::win32::WindowsStorageDaclPolicy policy(owner);
    for(const auto* relative:{L"Files\\General",L"Files\\Uploads",L"Files\\UploadToSysop",L"Data\\FileIntake",L"Data\\FileRemoved"}){
        const auto path=root/relative;if(absent(path))create_directory(path,descriptor.get());
        auto held=open_object(path,true);require(policy.assess(path,platform::win32::StorageDaclObjectKind::directory).status.succeeded(),"setup_file_store_boundary_unsafe_no_adoption");
    }
}
std::string operation_code(Operation operation) {
    switch (operation) {
    case Operation::fresh: return "fresh";
    case Operation::rebuild: return "rebuild";
    case Operation::maintenance: return "maintenance";
    case Operation::uninstall: return "uninstall";
    case Operation::uninstall_full: return "uninstall-full";
    case Operation::uninstall_missing: return "uninstall-missing";
    }
    throw std::runtime_error("setup_operation_invalid");
}
bool transaction_id(const std::wstring& text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F');
    });
}
bool backup_filename_valid(const Path& path) {
    const auto name = path.filename().native();
    constexpr wchar_t prefix[] = L"SFX-setup-backup-";
    if (name.size() != 36 || name.rfind(prefix, 0) != 0 ||
        name.substr(name.size() - 4) != L".zip") return false;
    for (std::size_t index = 17; index != 32; ++index) {
        if (index == 25) {
            if (name[index] != L'-') return false;
        } else if (name[index] < L'0' || name[index] > L'9') return false;
    }
    return true;
}
struct Channel {
    Handle pipe, peer;
    Pump pump;
    ULONGLONG session_start{GetTickCount64()};
    void tick() {
        require(!peer || WaitForSingleObject(peer.get(), 0) == WAIT_TIMEOUT, "setup_peer_exited");
        require(GetTickCount64() - session_start < installation_timeout_ms, "setup_session_timeout");
        require(!pump || pump(), "setup_cancelled");
    }
    void transfer(void* data, DWORD size, bool write) {
        auto* cursor = static_cast<BYTE*>(data); const auto start = GetTickCount64();
        while (size) {
            // Drain a queued final diagnostic even if the peer has just exited.
            require(!pump || pump(), "setup_cancelled"); DWORD done = 0;
            const auto ok = write ? WriteFile(pipe.get(), cursor, size, &done, nullptr)
                : ReadFile(pipe.get(), cursor, size, &done, nullptr);
            if (!ok) {
                const auto error = GetLastError();
                require(error == ERROR_NO_DATA || error == ERROR_PIPE_LISTENING, "setup_control_connection_lost");
            }
            require(done <= size, "setup_control_count_invalid");
            cursor += done; size -= done;
            if (!done) { tick(); require(GetTickCount64() - start < pipe_timeout_ms, "setup_control_timeout"); Sleep(20); }
        }
    }
    void send(const std::string& text) {
        require(!text.empty() && text.size() <= maximum_packet, "setup_control_oversized");
        auto size = static_cast<DWORD>(text.size()); transfer(&size, sizeof(size), true);
        transfer(const_cast<char*>(text.data()), size, true);
    }
    std::string receive() {
        DWORD size = 0; transfer(&size, sizeof(size), false);
        require(size && size <= maximum_packet, "setup_control_length_invalid");
        std::string result(size, '\0'); transfer(result.data(), size, false);
        require(std::all_of(result.begin(), result.end(), [](char c) { return c == '\n' || (c >= 32 && c < 127); }),
            "setup_control_character_invalid"); return result;
    }
    void await_initialized() {
        for (;;) { const auto reply = receive(); if (reply == "READY") return;
            require(reply == "BUSY", "setup_ordinary_initialization_failed"); }
    }
};
std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> result; std::size_t start = 0;
    for (;;) { const auto end = text.find('\n', start); result.push_back(text.substr(start, end - start));
        if (end == std::string::npos) break; start = end + 1; }
    return result;
}
Path display_source(const Path& root) {
    const bool legacy = !absent(root / L"Displays");
    require(!legacy || absent(root / L"Display"),
        "setup_display_directory_conflict_both_trees_preserved");
    return root / (legacy ? L"Displays" : L"Display");
}
std::vector<const wchar_t*> setup_mutable_directories(bool ftn_layout) {
    Receipt roles; roles.program_count=current_program_count; roles.ftn_layout=ftn_layout;
    return mutable_directories_for(roles);
}
void verify_mutable(const Path& root, const std::wstring& owner, bool legacy_display = false, bool ftn_layout = true) {
    platform::win32::WindowsStorageDaclPolicy policy(owner);
    for (auto leaf : setup_mutable_directories(ftn_layout)) {
        const auto path = legacy_display && !_wcsicmp(leaf, L"Display")
            ? display_source(root) : root / leaf;
        auto handle = open_object(path, true);
        require(policy.assess(path, platform::win32::StorageDaclObjectKind::directory).status.succeeded(),
            "setup_mutable_directory_security_invalid");
    }
}
void ordinary_publication_probe(const Path& root, const std::wstring& owner, const std::wstring& id) {
    require(!elevated(), "setup_content_must_be_unelevated");
    platform::win32::WindowsStorageDaclPolicy policy(owner);
    for (auto leaf : setup_mutable_directories(true)) {
        const auto first = root / leaf / (L".setup-" + id + L".writing");
        const auto second = root / leaf / (L".setup-" + id + L".probe");
        require(absent(first) && absent(second), "setup_probe_collision");
        void* raw = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"O:" + owner +
            L"D:P(A;;0x13019f;;;" + owner + L")(A;;FA;;;SY)(A;;FA;;;BA)").c_str(),
            SDDL_REVISION_1, &raw, nullptr), "setup_probe_descriptor_failed");
        Descriptor sd(raw);
        // Files get the accepted file-specific content descriptor after creation.
        create_file(first, {'S', 'F', 'X'}, sd.get());
        require(policy.apply(first, platform::win32::StorageDaclObjectKind::file).succeeded(), "setup_content_security_failed");
        require(MoveFileExW(first.c_str(), second.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            "setup_ordinary_publication_failed");
        require(read_file(second, 3) == std::vector<std::uint8_t>({'S', 'F', 'X'}) && DeleteFileW(second.c_str()),
            "setup_ordinary_publication_cleanup_failed");
    }
}
void make_private_directory(const Path& path, const std::wstring& owner) {
    auto sd = mutable_descriptor(owner); create_directory(path, sd.get());
}
void ensure_backup_destinations(const Path& root, const std::wstring& owner) {
    auto parent = open_object(root / L"Backup", true);
    for (const auto* name : {L"Upgrades", L"Scheduled"}) {
        const auto path = root / L"Backup" / name;
        if (absent(path)) make_private_directory(path, owner);
        auto pin = open_object(path, true);
        require(platform::win32::WindowsStorageDaclPolicy(owner).assess(path,
            platform::win32::StorageDaclObjectKind::directory).status.succeeded(),
            "setup_backup_destination_privacy_invalid_no_repair");
    }
}
void copy_content_file(const Path& from, const Path& to, const std::wstring& owner,
    const std::function<void()>& tick) {
    require(!elevated() && to.native().size() < 240, "setup_import_boundary_exceeded");
    tick();
    auto input = open_object(from, false, GENERIC_READ, FILE_SHARE_READ);
    require(absent(to), "setup_import_collision_source_preserved");
    require(CopyFileW(from.c_str(), to.c_str(), TRUE) != FALSE, "setup_import_copy_failed_source_preserved");
    platform::win32::WindowsStorageDaclPolicy policy(owner);
    // Clear only the imported read-only attribute, never source attributes.
    require(SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE &&
        policy.apply(to, platform::win32::StorageDaclObjectKind::file, false, true, false).succeeded(), "setup_import_security_failed");
    auto output = open_object(to, false, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ);
    LARGE_INTEGER a{}, b{};
    require(GetFileSizeEx(input.get(), &a) && GetFileSizeEx(output.get(), &b) && a.QuadPart == b.QuadPart,
        "setup_import_size_mismatch");
    std::array<BYTE, 65536> in{}, out{};
    for (;;) {
        tick(); DWORD got = 0, matched = 0;
        require(ReadFile(input.get(), in.data(), static_cast<DWORD>(in.size()), &got, nullptr) &&
            ReadFile(output.get(), out.data(), got, &matched, nullptr) && got == matched &&
            !std::memcmp(in.data(), out.data(), got), "setup_import_readback_failed");
        if (!got) break;
    }
    require(FlushFileBuffers(output.get()) != FALSE, "setup_import_flush_failed");
}
// Content-only ordinary copying. Source programs are never selected as payload.
// Preserve source ACLs/streams in the intact renamed source; new contents receive
// this installation's private identity policy, never adopted security metadata.
void copy_contents(const Path& source, const Path& destination, const std::wstring& owner,
    const std::function<void()>& tick, unsigned depth = 0) {
    require(!elevated() && depth < 64 && destination.native().size() < 240, "setup_import_boundary_exceeded");
    auto source_handle = open_object(source, true);
    auto destination_handle = open_object(destination, true);
    WIN32_FIND_DATAW item{}; const auto pattern = source / L"*";
    const auto search = FindFirstFileW(pattern.c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) {
        require(GetLastError() == ERROR_FILE_NOT_FOUND, "setup_source_enumeration_failed"); return;
    }
    struct SearchClose { HANDLE value; ~SearchClose() { FindClose(value); } } close{search};
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
        tick(); const auto from = source / item.cFileName; const auto to = destination / item.cFileName;
        require(!(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "setup_import_reparse_refused_source_preserved");
        require(absent(to), "setup_import_collision_source_preserved");
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            make_private_directory(to, owner); copy_contents(from, to, owner, tick, depth + 1);
        } else {
            copy_content_file(from, to, owner, tick);
        }
    } while (FindNextFileW(search, &item));
    require(GetLastError() == ERROR_NO_MORE_FILES, "setup_source_enumeration_incomplete");
}
bool mutable_name(const wchar_t* name) {
    return std::any_of(mutable_directories.begin(), mutable_directories.end(),
        [name](const wchar_t* candidate) { return !_wcsicmp(name, candidate); });
}
void recognize_source(const Path& root) {
    // Identification is NOT provenance. It only prevents a mistaken system or
    // unrelated folder selection from becoming a preservation/import operation.
    // Check content ancestors before opening even an ordinary imported file.
    auto root_handle = open_object(root, true);
    Handle config_handle, data_handle;
    if (!absent(root / L"Config")) config_handle = open_object(root / L"Config", true);
    if (!absent(root / L"Data")) data_handle = open_object(root / L"Data", true);
    const auto config = platform::win32::read_operational_configuration_file(root);
    const bool database_present = !absent(root / L"Data" / L"SFX.DB");
    if (database_present) {
        require(config.state == platform::win32::ConfigurationFileState::loaded,
            "setup_restore_valid_configuration_before_migration");
        auto data = open_object(root / L"Data", true);
        auto database = open_object(root / L"Data" / L"SFX.DB", false, GENERIC_READ, FILE_SHARE_READ);
        std::array<char, 16> header{}; DWORD bytes = 0;
        require(ReadFile(database.get(), header.data(), static_cast<DWORD>(header.size()), &bytes, nullptr) &&
            bytes == header.size() && !std::memcmp(header.data(), "SQLite format 3\0", 16),
            "setup_source_database_unrecognized");
        return;
    }
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((root / L"*").c_str(), &entry);
    require(search != INVALID_HANDLE_VALUE, "setup_source_unreadable");
    bool empty = true;
    do { if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) { empty = false; break; } }
    while (FindNextFileW(search, &entry));
    FindClose(search);
    if (empty) return;
    // An interrupted pre-account install has all fixed empty content roles.
    // It is preserved, never permission-repaired or certified from appearance.
    require(config.state == platform::win32::ConfigurationFileState::missing,
        "setup_source_database_missing_restore_backup");
    auto bin = open_object(root / L"Bin", true);
    for (const auto* leaf : mutable_directories) {
        if(!_wcsicmp(leaf,L"Node")&&absent(root/leaf))continue; // immutable pre-v8 source
        const auto current = (!_wcsicmp(leaf, L"Backup") && absent(root / leaf) &&
                              !absent(root / L"Backups"))
                                 ? root / L"Backups"
                                 : (!_wcsicmp(leaf, L"Display") ? display_source(root) : root / leaf);
        auto directory = open_object(current, true);
    }
}
void discard_work(const Path& directory, const std::function<void()>& tick, unsigned depth = 0) {
    require(!elevated() && depth < 64, "setup_work_cleanup_boundary_invalid");
    auto handle = open_object(directory, true);
    WIN32_FIND_DATAW item{}; const auto search = FindFirstFileW((directory / L"*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) { require(GetLastError() == ERROR_FILE_NOT_FOUND, "setup_work_read_failed"); return; }
    struct SearchClose { HANDLE value; ~SearchClose() { FindClose(value); } } close{search};
    do {
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
        tick(); const auto path = directory / item.cFileName;
        require(!(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "setup_work_reparse_refused");
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            discard_work(path, tick, depth + 1);
            require(RemoveDirectoryW(path.c_str()) != FALSE, "setup_work_directory_cleanup_failed");
        } else {
            auto file = open_object(path, false); file.reset();
            require(DeleteFileW(path.c_str()) != FALSE, "setup_work_file_cleanup_failed");
        }
    } while (FindNextFileW(search, &item));
    require(GetLastError() == ERROR_NO_MORE_FILES, "setup_work_enumeration_incomplete");
}
// Only Setup retires the obsolete installation subfolder, after its verified
// content backup. Runtime never treats it as a fallback or deletion target.
void retire_other_board_folder(const Path& root,const std::wstring& owner,
    const std::function<void()>& tick) {
    require(!elevated() && canonical_root(root)==root,"setup_conf_cleanup_root_invalid");
    const auto legacy=root/L"conf";
    require(legacy.parent_path()==root && legacy.filename()==L"conf","setup_conf_cleanup_target_invalid");
    if(absent(legacy))return;
    auto root_pin=open_object(root,true);
    auto legacy_pin=open_object(legacy,true);
    auto config_pin=open_object(root/L"Config",true);
    // Validate the complete tree before deleting any contents. Never follow
    // junctions, links or hardlinked files, even under the approved target.
    const std::function<void(const Path&,unsigned)> validate=[&](const Path& path,unsigned depth) {
        require(depth<64,"setup_conf_cleanup_depth_invalid");
        auto directory=open_object(path,true);
        for(const auto& entry:std::filesystem::directory_iterator(path)) {
            tick();const auto attributes=GetFileAttributesW(entry.path().c_str());
            require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"setup_conf_cleanup_redirect_refused");
            if(attributes&FILE_ATTRIBUTE_DIRECTORY)validate(entry.path(),depth+1);
            else {auto file=open_object(entry.path(),false);}
        }
    };
    validate(legacy,0);
    const auto source=legacy/L"otherbbs.txt",destination=root/L"Config"/L"otherbbs.txt";
    if(absent(destination) && !absent(source)) {
        const auto bytes=read_file(source,sfx::application::kOtherBoardTextMaximum);
        auto descriptor=mutable_descriptor(owner);create_file(destination,bytes,descriptor.get());
        require(platform::win32::WindowsStorageDaclPolicy(owner).apply(destination,platform::win32::StorageDaclObjectKind::file).succeeded(),"setup_otherbbs_move_security_failed");
        require(read_file(destination,sfx::application::kOtherBoardTextMaximum)==bytes,"setup_otherbbs_move_verification_failed");
    }
    discard_work(legacy,tick);
    legacy_pin.reset();
    require(RemoveDirectoryW(legacy.c_str())!=FALSE,"setup_conf_cleanup_directory_failed");
}
void import_source(const Plan& plan, const std::wstring& owner, const std::function<void()>& tick) {
    const auto displays = display_source(plan.staging);
    for (auto leaf : mutable_directories) {
        const auto source = !_wcsicmp(leaf, L"Display") ? displays : plan.staging / leaf;
        if (!_wcsicmp(leaf, L"Work") || !_wcsicmp(leaf, L"Backup") ||
            absent(source)) continue;
        copy_contents(source, plan.root / leaf, owner, tick);
    }
    const auto source_ini=plan.staging/L"Bin"/L"sfxexec.ini";
    if(!absent(source_ini)){
        auto source=open_object(source_ini,false,GENERIC_READ,FILE_SHARE_READ);
        const auto ini=read_file(source_ini,16384);auto target=open_object(plan.root/L"Bin"/L"sfxexec.ini",false,GENERIC_WRITE,0);DWORD written{};
        require(WriteFile(target.get(),ini.data(),static_cast<DWORD>(ini.size()),&written,nullptr)&&written==ini.size()&&SetEndOfFile(target.get())&&FlushFileBuffers(target.get()),"setup_door_ini_import_failed_source_preserved");
    }
    const auto legacy_list=plan.staging/L"conf"/L"otherbbs.txt";
    const auto current_list=plan.root/L"Config"/L"otherbbs.txt";
    if(absent(current_list) && !absent(legacy_list)) {
        auto legacy_pin=open_object(plan.staging/L"conf",true);
        auto config_pin=open_object(plan.root/L"Config",true);
        auto descriptor=mutable_descriptor(owner);
        create_file(current_list,read_file(legacy_list,sfx::application::kOtherBoardTextMaximum),descriptor.get());
        require(platform::win32::WindowsStorageDaclPolicy(owner).apply(current_list,platform::win32::StorageDaclObjectKind::file).succeeded(),"setup_import_otherbbs_security_failed");
    }
    // Do not reimport obsolete backup-history pools. The intact rebuild source
    // remains preserved separately inside Upgrades; it is never a deletion
    // target merely because it contains an old Backup/Backups directory.
    // Unknown root material stays intact in the verified source archive. It is
    // also imported below Backup, never promoted to the protected root/Bin.
    const auto unknown = plan.root / L"Backup" / L"Upgrades" / (L"ImportedRoot-" + plan.transaction);
    bool unknown_created = false;
    WIN32_FIND_DATAW item{}; const auto pattern = plan.staging / L"*";
    const auto search = FindFirstFileW(pattern.c_str(), &item);
    require(search != INVALID_HANDLE_VALUE, "setup_preserved_source_unavailable");
    struct SearchClose { HANDLE value; ~SearchClose() { FindClose(value); } } close{search};
    do {
        const auto* name = item.cFileName;
        if (!wcscmp(name, L".") || !wcscmp(name, L"..") || mutable_name(name) ||
            !_wcsicmp(name, L"Backups") || !_wcsicmp(name, L"Displays") ||
            !_wcsicmp(name, L"Bin") || !_wcsicmp(name, L"Temp") ||
            !_wcsicmp(name, L"SFXControlPanel.exe") || !_wcsicmp(name, L"SFXRuntime.exe") ||
            !_wcsicmp(name, L"SFX-BBS-Setup.exe") || !_wcsicmp(name, L"SFXConfig.exe")) continue;
        require(!(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "setup_unknown_reparse_refused");
        if (!unknown_created) { make_private_directory(unknown, owner); unknown_created = true; }
        const auto from = plan.staging / name; const auto to = unknown / name;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            make_private_directory(to, owner); copy_contents(from, to, owner, tick);
        } else {
            // Unknown root files need the same bounded read-back and flush as
            // files in known roles; preservation is not a substitute for a
            // verified destination copy.
            copy_content_file(from, to, owner, tick);
        }
    } while (FindNextFileW(search, &item));
    require(GetLastError() == ERROR_NO_MORE_FILES, "setup_unknown_enumeration_failed");
    // Configuration 4 stores no installation paths; its strict parser is reused.
    // Database, SSH identity and display resolution already use root-relative roles.
    const auto config = platform::win32::read_operational_configuration_file(plan.root);
    require(config.state == platform::win32::ConfigurationFileState::loaded ||
        config.state == platform::win32::ConfigurationFileState::missing, "setup_import_configuration_invalid_source_preserved");
}
#include "restore_artifacts.inc"
void create_content_backup(const Plan& plan, const std::wstring& owner,
                           const std::function<void()>& tick,MaintenanceProgress& progress) {
    archive_engine::CreateBackupRequest request;
    request.source_root = plan.operation == Operation::rebuild ? plan.staging : plan.root;
    // CVD definitions already contain compressed data. Storing those exact bytes
    // avoids recompression; other files use fast, compatible ZIP deflate. Every
    // source and every published member still receives full SHA-256 verification.
    request.compression_level=1;request.store_clamav_definitions=true;
    request.backup_progress=[&](const auto& event){progress.backup(event);};
    request.destination_path = plan.backup;
    request.partial_path = plan.backup.native() + std::wstring(L".partial");
    request.excluded_root_names = plan.operation == Operation::rebuild
                                      ? std::vector<std::string>{"Work"}
                                      : std::vector<std::string>{"Backup", "Work", "Bin"};
    if (plan.operation == Operation::maintenance) {
        request.included_root_names = {
            "Config", "Data", "Display", "Displays", "Doors", "Logs", "BotGate", "conf"};
        if(plan.previous && plan.previous->ftn_layout) {
            verify_ftn_directory(*plan.previous);
            request.included_root_names.push_back("FTN");
        }
        request.excluded_relative_paths={"Data/SFXRuntime.lock","Data/FileIntake","Data/FileRemoved"};
    }
    request.metadata.backup_type = plan.operation == Operation::rebuild
                                       ? "setup-migration-source"
                                       : "setup-upgrade";
    request.metadata.transaction_id = ascii(plan.transaction);
    request.metadata.sfx_version = SFX_PRODUCT_VERSION_SHORT_STRING;
    request.metadata.source_generation =
        plan.previous ? std::to_string(plan.previous->generation) : "unknown";
    request.metadata.created = plan.backup_timestamp;
    request.cancelled = [&] {
        try {
            tick();
            return false;
        } catch (...) {
            return true;
        }
    };
    std::unique_ptr<platform::win32::FileBackupCapture> contents;
    std::unique_ptr<platform::win32::FileBackupSnapshot> identity_cleanup;
    std::optional<platform::win32::BackupCollection> collection;
    // A receipt-verified installation can legitimately be updated before its
    // first Runtime bootstrap. With no database there is no saved area-content
    // selection to capture. Still preserve all existing core files below; do
    // not create a database, invent area choices or silently include payloads.
    if(plan.operation==Operation::maintenance&&!absent(plan.root/L"Data"/L"SFX.DB")){
        (void)platform::win32::configure_portable_file_bindings(plan.root);
        progress.status(L"Preparing backup: capturing selected File Area contents");
        collection.emplace(platform::win32::open_backup_collection(plan.root,owner,archive_engine::BackupKind::setup_upgrade));
        const auto config=platform::win32::read_backup_file_configuration(plan.root,owner);
        contents=platform::win32::FileBackupCapture::begin(plan.root,owner,config,*collection,archive_engine::BackupKind::setup_upgrade,request.cancelled);
        archive_engine::ManagedBackupRequest binding;binding.installation_root=plan.root;
        contents->bind(binding,platform::win32::read_backup_file_configuration(plan.root,owner));request.source_overrides=binding.file_contents;
        // Preserve genuinely legacy pre-update backups without inventing a
        // key. An established portable board carries a current pinned map.
        if(platform::win32::load_portable_board_key(plan.root)){
            const auto identity_path=plan.root/L"Work"/(L"file-identities-setup-"+plan.transaction+L".snapshot");
            const auto identities=platform::win32::refresh_portable_file_snapshot(plan.root,owner,false,identity_path,request.cancelled);
            if(!identities.published)throw std::runtime_error("setup_backup_file_identity_snapshot_incomplete_preserved"+
                (identities.diagnostics.empty()?std::string{}:std::string(": ")+identities.diagnostics.front()));
            identity_cleanup=std::make_unique<platform::win32::FileBackupSnapshot>(platform::win32::FilePayloadDirectory::private_store(plan.root/L"Work",owner),identity_path.filename().u8string());
            request.excluded_relative_paths.push_back("Config/file-identities.v1");
            request.source_overrides.push_back({"Config/file-identities.v1",identity_path});
        }
    }
    const auto result = archive_engine::create_backup(request);
    if (!result.ok()) {
        throw std::runtime_error(std::string("setup_backup_") +
                                 archive_engine::error_name(result.error));
    }
    platform::win32::WindowsStorageDaclPolicy policy(owner);
    require(policy.apply(plan.backup, platform::win32::StorageDaclObjectKind::file).succeeded(),
            "setup_backup_security_failed");
    progress.status(L"Verifying published backup");
    require(archive_engine::inspect_backup(plan.backup,{},request.cancelled,[&](const auto& event){progress.backup(event,true);}).result.ok(),
            "setup_backup_post_security_verification_failed");
    require(!contents||contents->cleanup(),"setup_backup_content_scratch_retained");
    require(!identity_cleanup||identity_cleanup->cleanup(),"setup_backup_identity_scratch_retained");
}
struct RetiredObject {
    Path path;
    Handle handle;
};

// Collect and compare the ENTIRE tree before deleting any original. All
// originals remain pinned against writes/rename, including directory ancestors.
// The mutable copy is preservation only, never executable installation authority.
void verify_retirable_copy(const Path& original, const Path& copy,
    std::vector<RetiredObject>& objects, const std::function<void()>& tick,
    const std::wstring& owner, unsigned depth = 0) {
    require(depth < 64 && objects.size() < 10000, "setup_history_migration_limit_originals_preserved");
    tick();
    const auto attributes = GetFileAttributesW(original.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "setup_history_redirect_originals_preserved");
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto old = open_object(original, directory, GENERIC_READ | READ_CONTROL | DELETE,
        FILE_SHARE_READ | (directory ? FILE_SHARE_WRITE : 0));
    auto saved = open_object(copy, directory, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    require(platform::win32::WindowsStorageDaclPolicy(owner).assess(copy,
        directory ? platform::win32::StorageDaclObjectKind::directory :
                    platform::win32::StorageDaclObjectKind::file).status.succeeded(),
        "setup_history_copy_privacy_invalid_originals_preserved");
    if (directory) {
        std::vector<std::wstring> names;
        for (const auto& child : fs::directory_iterator(original)) {
            names.push_back(child.path().filename().native());
            verify_retirable_copy(child.path(), copy / child.path().filename(), objects, tick, owner, depth + 1);
        }
        std::vector<std::wstring> saved_names;
        for (const auto& child : fs::directory_iterator(copy)) saved_names.push_back(child.path().filename().native());
        std::sort(names.begin(), names.end()); std::sort(saved_names.begin(), saved_names.end());
        require(names == saved_names, "setup_history_copy_inventory_mismatch_originals_preserved");
    } else {
        LARGE_INTEGER a{}, b{};
        require(GetFileSizeEx(old.get(), &a) && GetFileSizeEx(saved.get(), &b) && a.QuadPart == b.QuadPart,
            "setup_history_copy_size_mismatch_originals_preserved");
        std::array<BYTE, 65536> before{}, after{};
        for (;;) {
            tick(); DWORD read{}, copied{};
            require(ReadFile(old.get(), before.data(), static_cast<DWORD>(before.size()), &read, nullptr) &&
                ReadFile(saved.get(), after.data(), read, &copied, nullptr) && read == copied &&
                !std::memcmp(before.data(), after.data(), read), "setup_history_copy_changed_originals_preserved");
            if (!read) break;
        }
    }
    // Keep the verified copy pinned too. Otherwise an operator-level process
    // could alter preservation after comparison while elevated cleanup runs.
    objects.push_back({{}, std::move(saved)});
    objects.push_back({original, std::move(old)}); // children before parents
}

void retire_committed_conf(const Plan& plan, const std::wstring& owner,
    const std::function<void()>& tick) {
    const auto original = plan.root / L"conf";
    if (absent(original)) return;
    std::vector<RetiredObject> objects;
    verify_retirable_copy(original, plan.root / L"Backup" / L"Upgrades" / (L"Source-conf-" + plan.transaction), objects, tick, owner);
    const auto source = original / L"otherbbs.txt", destination = plan.root / L"Config" / L"otherbbs.txt";
    if (absent(destination) && !absent(source)) {
        // The source is already held with DELETE authority; a second read must
        // allow that held access while copying its verified private contents.
        const auto bytes = [&] {
            auto file = open_object(source, false, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE);
            LARGE_INTEGER size{};
            require(GetFileSizeEx(file.get(), &size) && size.QuadPart >= 0 &&
                size.QuadPart <= sfx::application::kOtherBoardTextMaximum, "setup_otherbbs_migration_size_invalid");
            std::vector<std::uint8_t> value(static_cast<std::size_t>(size.QuadPart)); DWORD read{};
            require(ReadFile(file.get(), value.data(), static_cast<DWORD>(value.size()), &read, nullptr) &&
                read == value.size(), "setup_otherbbs_migration_read_failed");
            return value;
        }();
        auto descriptor = mutable_descriptor(owner);
        create_file(destination, bytes, descriptor.get());
        require(platform::win32::WindowsStorageDaclPolicy(owner).apply(destination,
            platform::win32::StorageDaclObjectKind::file).succeeded(), "setup_otherbbs_migration_security_failed");
        require(read_file(destination, sfx::application::kOtherBoardTextMaximum) == bytes,
            "setup_otherbbs_migration_copy_mismatch");
    }
    for (auto& object : objects) {
        tick(); if (object.path.empty()) continue;
        FILE_DISPOSITION_INFO disposition{TRUE};
        require(SetFileInformationByHandle(object.handle.get(), FileDispositionInfo, &disposition,
            sizeof(disposition)) != FALSE, "setup_conf_retirement_failed_copy_preserved");
        object.handle.reset();
    }
}
std::wstring shortcut_name(const Plan& plan) {
    return L"Spitfire-X BBS (" + plan.root.filename().native() + L"-" + plan.transaction.substr(0, 8) + L").lnk";
}
std::wstring configuration_shortcut_name(const Plan& plan) {
    return L"Spitfire-X BBS Configuration (" + plan.root.filename().native() +
        L"-" + plan.transaction.substr(0, 8) + L").lnk";
}
std::wstring file_manager_shortcut_name(const Plan& plan){return L"Spitfire-X File Manager ("+plan.root.filename().native()+L"-"+plan.transaction.substr(0,8)+L").lnk";}
Path known_folder(int folder) {
    std::array<wchar_t, MAX_PATH> path{};
    require(SHGetFolderPathW(nullptr, folder, nullptr, SHGFP_TYPE_CURRENT, path.data()) == S_OK,
        "setup_shortcut_folder_unavailable"); return path.data();
}
void create_shortcut(const Path& path, const Path& target, const std::wstring& arguments = {}) {
    require(absent(path), "setup_shortcut_collision");
    const auto temporary = path.parent_path() / (L"SFX-shortcut-" + unique_id() + L".tmp");
    create_file(temporary, {}, mutable_descriptor(operator_sid()).get());
    struct Cleanup { Path path; ~Cleanup() { if (!path.empty()) DeleteFileW(path.c_str()); } } cleanup{temporary};
    IShellLinkW* link = nullptr;
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
        reinterpret_cast<void**>(&link))), "setup_shortcut_create_failed");
    struct Release { IShellLinkW* link; ~Release() { link->Release(); } } release{link};
    require(SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link->SetWorkingDirectory(target.parent_path().c_str())) &&
        (arguments.empty() || SUCCEEDED(link->SetArguments(arguments.c_str()))) &&
        SUCCEEDED(link->SetDescription(SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE)), "setup_shortcut_target_failed");
    IPersistFile* file = nullptr;
    require(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))), "setup_shortcut_persistence_failed");
    const auto result = file->Save(temporary.c_str(), TRUE); file->Release();
    require(SUCCEEDED(result), "setup_shortcut_save_failed");
    require(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
        "setup_shortcut_collision_preserved");
    cleanup.path.clear();
}
void remove_shortcut_if_matching(const Path& path, const Path& target) {
    if (absent(path)) return;
    auto handle = open_object(path, false);
    IShellLinkW* link = nullptr;
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
        reinterpret_cast<void**>(&link))), "setup_shortcut_open_failed");
    struct Release { IShellLinkW* link; ~Release() { link->Release(); } } release{link};
    IPersistFile* file = nullptr;
    require(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))), "setup_shortcut_query_failed");
    const auto loaded = file->Load(path.c_str(), STGM_READ); file->Release();
    std::array<wchar_t, MAX_PATH> actual{};
    if (FAILED(loaded) || FAILED(link->GetPath(actual.data(), static_cast<int>(actual.size()), nullptr, SLGP_RAWPATH)) ||
        _wcsicmp(actual.data(), target.c_str())) return; // unrelated link is never removed.
    handle.reset(); require(DeleteFileW(path.c_str()) != FALSE, "setup_shortcut_removal_failed");
}
#include "desktop_shortcuts.inc"
#include "uninstall_registration.inc"

void reconcile_start_menu_shortcut(const Path& folder,const Path& target,const std::wstring& name,
    std::wstring_view caption,const std::wstring& identity){
    const auto candidates=desktop_candidates(folder,L"*.lnk");
    const auto current=folder/name;
    const auto root=target.parent_path().parent_path();
    Path chosen;
    for(const auto& path:candidates){
        if(!owned_desktop_shortcut(path,target,identity,false,nullptr,true))continue;
        if(!legacy_shortcut_name(path,caption,root)||!owned_desktop_shortcut(path,target,identity,true)){
            chosen=path;
            break;
        }
        if(chosen.empty()||path==current)chosen=path;
    }
    if(chosen.empty()){
        if(!absent(current))return;
        create_shortcut(current,target);
        chosen=current;
    }
    for(const auto& path:candidates){
        if(path!=chosen&&legacy_shortcut_name(path,caption,root))
            retire_owned_desktop_shortcut(path,target,identity,true);
    }
}
void shortcut_changes(const Plan& plan, const Options& options) {
    // Stable installed identity, not transaction ID, keeps maintenance idempotent.
    Plan naming = plan;
    if (removing(plan.operation)) {
        require(plan.previous.has_value(), "setup_uninstall_shortcut_receipt_missing");
        naming.transaction = plan.previous->id;
    } else {
        const auto id_bytes = read_file(plan.root / L"Bin" / L"installation.id", 32);
        naming.transaction.assign(id_bytes.begin(), id_bytes.end());
    }
    const auto name = shortcut_name(naming);
    const auto configuration_name = configuration_shortcut_name(naming);
    const auto manager_name=file_manager_shortcut_name(naming);
    const auto target = plan.root / L"Bin" / programs[control_panel_program_index];
    const auto configuration_target = plan.root / L"Bin" / programs[configuration_program_index];
    const auto manager_target=plan.root/L"Bin"/programs[file_manager_program_index];
    for (const auto folder : {CSIDL_PROGRAMS, CSIDL_DESKTOPDIRECTORY}) {
        if(folder==CSIDL_DESKTOPDIRECTORY){
            desktop_shortcut_changes(known_folder(folder),target,name,naming.transaction,
                removing(plan.operation),options.desktop);
            continue;
        }
        const auto path = known_folder(folder) / name;
        if (removing(plan.operation)) {
            remove_shortcut_if_matching(path, target);
            if (folder == CSIDL_PROGRAMS)
                remove_shortcut_if_matching(known_folder(folder) / configuration_name,
                    configuration_target);
            if(folder==CSIDL_PROGRAMS)remove_shortcut_if_matching(known_folder(folder)/manager_name,manager_target);
            for(const auto& candidate:desktop_candidates(known_folder(folder),L"Spitfire-X*.lnk")){
                if(legacy_shortcut_name(candidate,L"Spitfire-X BBS",plan.root))
                    retire_owned_desktop_shortcut(candidate,target,naming.transaction,true);
                else if(legacy_shortcut_name(candidate,L"Spitfire-X BBS Configuration",plan.root))
                    retire_owned_desktop_shortcut(candidate,configuration_target,naming.transaction,true);
                else if(legacy_shortcut_name(candidate,L"Spitfire-X File Manager",plan.root))
                    retire_owned_desktop_shortcut(candidate,manager_target,naming.transaction,true);
            }
        } else if (options.start_menu) {
            const auto programs_folder=known_folder(folder);
            reconcile_start_menu_shortcut(programs_folder,target,name,L"Spitfire-X BBS",naming.transaction);
            reconcile_start_menu_shortcut(programs_folder,configuration_target,configuration_name,
                L"Spitfire-X BBS Configuration",naming.transaction);
            reconcile_start_menu_shortcut(programs_folder,manager_target,manager_name,
                L"Spitfire-X File Manager",naming.transaction);
        }
    }
}
void finish_service_setup(const Plan& plan,const std::wstring& owner,Channel& channel,const std::optional<bool>& selected) {
    require(!selected,"setup_mode_changes_use_configuration");
    const auto registration=read_service_registration(plan.root,true);
    const bool wanted=registration&&registration->enabled;
    if(wanted) {channel.send("SERVICE-PREPARE");channel.await_initialized();}
    ensure_owned_service(plan.root,owner);
}
void install_programs(const Plan& plan, const std::wstring& owner, Channel& channel, HANDLE ordinary_token,
    const Path& clamav_package, const std::optional<bool>& service_mode, bool install_uninstall_entry) {
    const auto media = payload(); auto structural = structural_descriptor(owner);
    const auto component_location=system_door_component_location();auto component_sd=shared_component_descriptor();
    if(!component_location.image.empty())DoorComponentChange::recover(component_location,component_sd.get(),component_generation_committed);
    std::unique_ptr<DoorComponentChange> component;
    if (plan.operation == Operation::fresh || plan.operation == Operation::rebuild) {
        if (plan.operation == Operation::rebuild) {
            // Preserve only: no permission rewrite, traversal, source execution or adoption.
            auto source = open_object(plan.root, true);
            require(absent(plan.staging), "setup_preservation_collision"); source.reset();
            require(MoveFileExW(plan.root.c_str(), plan.staging.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
                "setup_source_preservation_failed_no_repair");
        }
        create_directory(plan.root, structural.get());
        create_directory(plan.root / L"Bin", structural.get());
        create_directory(plan.root / L"Bin" / L"Tools", structural.get());
        auto mutable_sd = mutable_descriptor(owner);
        for (auto leaf : setup_mutable_directories(true)) create_directory(plan.root / leaf, mutable_sd.get());
        const auto id = ascii(plan.transaction);
        create_file(plan.root / L"Bin" / L"installation.id", {id.begin(), id.end()}, structural.get());
        if(plan.operation==Operation::fresh&&!starter_door_assets.empty())begin_fresh_starter_copy(plan.root,owner,id);
        for (std::size_t n = 0; n != current_program_count; ++n)
            create_file(plan.root / L"Bin" / programs[n], media[n], structural.get());
        carry_portable_dos_input(plan.root,owner);
        seed_door_ini(plan.root,owner);
        if(plan.operation==Operation::fresh&&!starter_door_assets.empty()) {
            std::vector<StarterDoorFile> files;
            for(const auto& asset:starter_door_assets)
                files.push_back({asset.filename,resource(asset.resource_id),asset.sha256});
            install_fresh_starter_doors(plan.root,owner,id,files);
        }
        if(!clamav_package.empty())platform::win32::provision_clamav_engine(plan.root,owner,plan.transaction,clamav_package,[&]{channel.tick();});
        auto receipt = inspect_generation(plan.root, owner, plan.transaction, 1, true, current_program_count, true);
        channel.send("INITIALIZE"); channel.await_initialized();
        if (plan.operation == Operation::rebuild) {
            if(!starter_door_assets.empty()) {
                std::vector<StarterDoorFile> files;
                for(const auto& asset:starter_door_assets)
                    files.push_back({asset.filename,resource(asset.resource_id),asset.sha256});
                resume_owned_starter_doors(plan.root,plan.staging,owner,id,files);
            }
            // Remove the root-level staging name atomically without executing or
            // adopting it. The intact source remains below Backup as additional
            // recovery material beside the independently verified ZIP.
            const auto retained = plan.root / L"Backup" / L"Upgrades" /
                                  (L"Migration-Source-" + plan.transaction);
            require(absent(retained), "setup_migration_retention_collision");
            require(MoveFileExW(plan.staging.c_str(), retained.c_str(),
                                MOVEFILE_WRITE_THROUGH) != FALSE,
                    "setup_migration_staging_relocation_failed_archive_preserved");
        }
        seed_missing_sysop_docs(plan.root,owner);
        verify_mutable(plan.root, owner); verify_structure(receipt);
        if(!component_location.image.empty()){channel.send("DOS-COMPONENT");component=DoorComponentChange::prepare(component_location,receipt,resource(240),door_component_policy(),component_sd.get());receipt.door_component=component->identity();}
        create_receipt(receipt); // Never created for a partial copy or failed ordinary initialization.
        commit_starter_door_copy(plan.root,owner);
        if(component)component->commit();
        finish_service_setup(plan,owner,channel,service_mode);
        if (install_uninstall_entry) publish_uninstall_entry(receipt, ordinary_token);
        channel.send("COMMITTED"); return;
    }
    require(plan.previous.has_value() && plan.previous->committed, "setup_previous_generation_unavailable");
    auto previous = *plan.previous;
    if (removing(plan.operation)) {
        const bool missing = plan.operation == Operation::uninstall_missing;
        const bool full = plan.operation == Operation::uninstall_full;
        if (missing) require_missing_installation(previous);
        else verify_structure(previous);
        // Collect only protected same-owner history for this native volume and
        // exact recorded path. Available registrations always retain priority.
        std::vector<Receipt> obsolete;
        bool retained_history = false;
        for (const auto& record : registered_installations({}, true, previous.root)) {
            const auto& old = record.receipt;
            if (old.id == previous.id || _wcsicmp(old.root.c_str(), previous.root.c_str())) continue;
            if (old.committed && !record.available && old.volume == previous.volume &&
                (missing || old.root_id != previous.root_id || old.bin_id != previous.bin_id))
                obsolete.push_back(old);
            else retained_history = true;
        }
        if (retained_history) channel.send("UNINSTALL-HISTORY-RETAINED");
        if (full) remove_board_tree(plan.root, [&]{channel.tick();}, false);
        for (const auto& old : obsolete) {
            remove_obsolete_owned_service(old);
            channel.send("UNINSTALL-HISTORY " + ascii(old.id)); channel.await_initialized();
            unregister_app(old);
            if (old.door_component.registered) {
                require(!component_location.image.empty(),"setup_dos_component_platform_changed_preserved");
                auto retire = DoorComponentChange::prepare(component_location,old,{},door_component_policy(),component_sd.get(),true);
                remove_receipt(old);
                retire->commit();
            } else remove_receipt(old);
        }
        if (missing) {
            remove_obsolete_owned_service(previous);
            channel.send("UNINSTALL"); channel.await_initialized();
            unregister_app(previous);
            if (previous.door_component.registered) {
                require(!component_location.image.empty(),"setup_dos_component_platform_changed_preserved");
                component=DoorComponentChange::prepare(component_location,previous,{},door_component_policy(),component_sd.get(),true);
            }
            require_missing_installation(previous);
            remove_receipt(previous);
            if(component)component->commit();
            channel.send("REMOVED"); return;
        }
        for (std::size_t n = 0; n != previous.program_count; ++n) {
            const auto program = plan.root / L"Bin" / programs[n];
            auto file = open_object(program, false, DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            require(file_id(file.get()) == previous.program_ids[n] && exact_security(file.get(),structural.get()),
                "setup_uninstall_program_identity_changed");
        }
        remove_owned_service(plan.root,owner);
        if(previous.door_component.registered){require(!component_location.image.empty(),"setup_dos_component_platform_changed_preserved");component=DoorComponentChange::prepare(component_location,previous,{},door_component_policy(),component_sd.get(),true);}
        if(platform::win32::windows7_no_av()){
            auto retirement=platform::win32::ClamAvRetirement::prepare(plan.root,owner,previous.id);
            retirement->retire();
        }
        // Ordinary shortcut removal finishes before the protected identity file
        // can disappear. It never runs before actual Windows approval.
        channel.send("UNINSTALL"); channel.await_initialized();
        const auto local = local_uninstall_link(previous);
        if (!absent(local) && selected_uninstall_link(local, previous))
            require(DeleteFileW(local.c_str()), "setup_uninstall_link_remove_failed");
        auto incomplete = previous; incomplete.committed = false;
        replace_receipt(previous, incomplete);
        // Only the exact known installed program roles are removed. Data and
        // unknown root/Bin material are intentionally left intact.
        for (std::size_t n = 0; n != previous.program_count; ++n) {
            auto file = open_object(plan.root / L"Bin" / programs[n], false,
                DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ);
            require(file_id(file.get()) == previous.program_ids[n] && exact_security(file.get(),structural.get()),
                "setup_uninstall_program_identity_changed");
            FILE_DISPOSITION_INFO disposition{TRUE};
            require(SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)),
                "setup_uninstall_program_failed");
            file.reset();
        }
        if (previous.program_count >= archive_program_count && !platform::win32::clamav_installed(plan.root) &&
            !RemoveDirectoryW((plan.root / L"Bin" / L"Tools").c_str()))
            require(GetLastError() == ERROR_DIR_NOT_EMPTY, "setup_uninstall_tools_directory_failed");
        // The service-removal journal kept both Runtime entry paths blocked
        // until the owned program generation was invalidated and removed.
        for(const auto* leaf:{L"service-registration.v2",L"service-registration.pending",L"service-mode.v1",L"service-mode.pending"}){
            const auto path=plan.root/L"Bin"/leaf;
            if(!absent(path)){
                auto file=open_object(path,false);require(exact_security(file.get(),structural.get()),"setup_service_metadata_security_invalid");file.reset();
                require(DeleteFileW(path.c_str()),"setup_service_metadata_remove_failed");
            }
        }
        require(DeleteFileW((plan.root / L"Bin" / L"installation.id").c_str()) != FALSE,
            "setup_uninstall_identity_failed");
        if (full) {
            channel.send("REMOVE-BOARD");
            remove_board_tree(plan.root, [&]{channel.tick();}, true);
            require(absent(plan.root), "setup_full_removal_folder_retained");
        }
        remove_receipt(incomplete);
        if(component){component->commit();if(component->image_retained())channel.send("DOS-RETAINED");}
        unregister_app(previous);
        channel.send("REMOVED"); return;
    }
    verify_structure(previous);
    require(previous.generation < std::numeric_limits<std::uint64_t>::max(), "setup_generation_exhausted");
    // A name/ACL is not provenance. An old installation may not adopt an
    // unexpected FTN directory, even if it appears empty or owner-private.
    require(previous.ftn_layout || absent(plan.root/L"FTN"), "setup_untracked_ftn_directory_preserved_repair_required");
    for(std::size_t n=previous.program_count;n<current_program_count;++n)
        require(absent(plan.root/L"Bin"/programs[n]),"setup_new_program_destination_occupied_source_preserved");
    if (absent(plan.root / L"Backup")) {
        auto mutable_sd = mutable_descriptor(owner);
        create_directory(plan.root / L"Backup", mutable_sd.get());
    }
    if (absent(plan.root/L"BotGate")) {
        auto mutable_sd=mutable_descriptor(owner);
        create_directory(plan.root/L"BotGate",mutable_sd.get());
    }
    if(absent(plan.root/L"Node")){auto mutable_sd=mutable_descriptor(owner);create_directory(plan.root/L"Node",mutable_sd.get());}
    verify_mutable(plan.root, owner, true, previous.ftn_layout);
    channel.send("BACKUP"); channel.await_initialized();
    std::optional<std::wstring> clamav_predecessor;
    if (platform::win32::clamav_available() && platform::win32::clamav_installed(plan.root)) {
        const char* status = "CLAMAV-VERIFIED";
        try {
            clamav_predecessor = platform::win32::confirmed_preparation_predecessor(previous);
            const auto predecessor = clamav_predecessor.value_or(std::wstring{});
            if (platform::win32::clamav_upgrade_required(plan.root,owner,previous.id,predecessor)) {
                status = "CLAMAV-UPGRADE-REQUIRED";
                if (!predecessor.empty()&&!absent(plan.root/L"Bin"/L"Tools"/L"ClamAV"))
                    platform::win32::rebind_clamav_receipt(plan.root,owner,previous.id,predecessor,{},[&]{channel.tick();});
            } else if (clamav_predecessor) {
                const auto result = platform::win32::rebind_clamav_receipt(plan.root, owner, previous.id,
                    predecessor, {}, [&] { channel.tick(); });
                if (result.configuration_preserved) status = "CLAMAV-CONFIG-PRESERVED";
                else if (result.receipt_changed || result.configuration_changed) status = "CLAMAV-REPAIRED";
            } else {
                platform::win32::verify_clamav_installation(plan.root, owner, previous.id);
            }
        } catch (const std::exception&) {
            status = "CLAMAV-UNVERIFIED";
        }
        channel.send(status);
    }
    channel.send("PREPARE-PROGRAMS");
    // A distinct protected transaction staging, never a Bin history collection.
    // Incomplete/failed transactions retain this exact recovery material.
    auto backup_parent=open_object(plan.root/L"Backup",true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    relocate_restore_artifacts(plan,owner);
    const auto recovery = plan.root / L"Backup" / L"Recovery";
    if (absent(recovery)) create_directory(recovery, structural.get());
    auto recovery_anchor = open_object(recovery, true,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    require(exact_security(recovery_anchor.get(), structural.get()), "setup_recovery_boundary_invalid");
    const auto backup = recovery / (L"Setup-" + plan.transaction);
    create_directory(backup, structural.get());
    if (previous.program_count >= archive_program_count)
        create_directory(backup / L"Tools", structural.get());
    std::array<std::vector<std::uint8_t>, current_program_count> old_programs;
    for (std::size_t n = 0; n != previous.program_count; ++n) {
        old_programs[n] = read_file(plan.root / L"Bin" / programs[n], maximum_program_bytes);
        require(sha256(old_programs[n]) == previous.program_hashes[n], "setup_old_program_changed");
        create_file(backup / programs[n], old_programs[n], structural.get());
    }
    create_file(backup / L"receipt.recovery", encode_receipt(previous), structural.get());
    auto incomplete = previous; incomplete.committed = false;
    replace_receipt(previous, incomplete); // Survives process loss before either replacement.
    Receipt current;
    std::unique_ptr<platform::win32::ClamAvRetirement> retirement;
    bool created_tools = false;
    bool renamed_display = false;
    std::optional<Receipt> introduced_ftn;
    std::array<bool,current_program_count> introduced{},staged{};
    try {
        channel.send("REPLACE-PROGRAMS");
        if(!previous.ftn_layout) {
            auto mutable_sd=mutable_descriptor(owner);
            create_directory(plan.root/L"FTN",mutable_sd.get());
            auto bound=previous; bound.program_count=current_program_count; bound.ftn_layout=true;
            auto directory=open_object(plan.root/L"FTN",true);
            bound.ftn_volume=volume_serial(directory.get()); bound.ftn_directory_id=file_id(directory.get());
            introduced_ftn=bound;
            verify_ftn_directory(*introduced_ftn);
        }
        if (display_source(plan.root).filename() == L"Displays") {
            require(MoveFileExW((plan.root / L"Displays").c_str(),
                               (plan.root / L"Display").c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
                    "setup_display_migration_failed_source_preserved");
            renamed_display = true;
        }
        verify_mutable(plan.root, owner);
        if (previous.program_count == legacy_program_count) {
            require(absent(plan.root / L"Bin" / L"Tools"),
                    "setup_untracked_tools_directory_refused");
            create_directory(plan.root / L"Bin" / L"Tools", structural.get());
            created_tools = true;
        }
        for (std::size_t n = 0; n != current_program_count; ++n) {
            channel.tick();
            const auto pending = plan.root / L"Bin" / (std::wstring(programs[n]) + L".new-" + plan.transaction);
            create_file(pending, media[n], structural.get());
            staged[n]=true;
            require(file_sha256(pending) == sha256(media[n]), "setup_staged_program_mismatch");
            require(MoveFileExW(pending.c_str(), (plan.root / L"Bin" / programs[n]).c_str(),
                MOVEFILE_WRITE_THROUGH | (n<previous.program_count?MOVEFILE_REPLACE_EXISTING:0)) != FALSE, "setup_program_replacement_failed");
            if(n>=previous.program_count)introduced[n]=true;
        }
        seed_missing_sysop_docs(plan.root,owner);
        channel.send("VERIFY-PROGRAMS");
        carry_portable_dos_input(plan.root,owner);
        current = inspect_generation(plan.root, owner, previous.id, previous.generation + 1, true, current_program_count, true);
        current.door_component=previous.door_component;
        channel.send("VERIFY-FTN"); channel.await_initialized();
        seed_door_ini(plan.root,owner);
        if(!component_location.image.empty()){channel.send("DOS-COMPONENT");component=DoorComponentChange::prepare(component_location,current,resource(240),door_component_policy(),component_sd.get());current.door_component=component->identity();}
        if(platform::win32::windows7_no_av())retirement=platform::win32::ClamAvRetirement::prepare(plan.root,owner,previous.id);
    } catch (...) {
        try{channel.send("ROLLBACK-PROGRAMS");}catch(...){} // reporting cannot block rollback
        if(component){component->rollback();component.reset();}
        else if(!component_location.image.empty())DoorComponentChange::recover(component_location,component_sd.get(),component_generation_committed);
        // No success is reported after a failed update, even if rollback succeeds.
        // If any restoration fails, the durable incomplete record remains closed.
        for (std::size_t n = 0; n != previous.program_count; ++n) {
            const auto pending = plan.root / L"Bin" / (std::wstring(programs[n]) + L".restore-" + plan.transaction);
            create_file(pending, old_programs[n], structural.get());
            require(MoveFileExW(pending.c_str(), (plan.root / L"Bin" / programs[n]).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE, "setup_rollback_incomplete_source_preserved");
        }
        for (std::size_t n = previous.program_count; n != current_program_count; ++n) {
            const auto installed = plan.root / L"Bin" / programs[n];
            const Path pending = installed.native() + L".new-" + plan.transaction;
            require(!introduced[n] || absent(installed) || DeleteFileW(installed.c_str()) != FALSE,
                    "setup_new_helper_rollback_failed");
            require(!staged[n] || absent(pending) || DeleteFileW(pending.c_str()) != FALSE,
                    "setup_pending_helper_rollback_failed");
        }
        if (created_tools)
            require(RemoveDirectoryW((plan.root / L"Bin" / L"Tools").c_str()) != FALSE,
                    "setup_tools_directory_rollback_failed");
        if (renamed_display)
            require(MoveFileExW((plan.root / L"Display").c_str(),
                               (plan.root / L"Displays").c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
                    "setup_display_rollback_incomplete_backup_preserved");
        if(introduced_ftn) {
            verify_ftn_directory(*introduced_ftn);
            // Empty-only rollback. Any mail or unexpected content keeps the
            // incomplete receipt and directory intact for explicit recovery.
            require(RemoveDirectoryW((plan.root/L"FTN").c_str())!=FALSE,
                "setup_ftn_rollback_nonempty_preserved_recovery_required");
        }
        auto restored = inspect_generation(plan.root, owner, previous.id,
                                                  previous.generation + 1, true,
                                                  previous.program_count, previous.ftn_layout);
        restored.door_component=previous.door_component;
        if(previous.door_component.registered){component=DoorComponentChange::prepare(component_location,restored,resource(240),door_component_policy(),component_sd.get());restored.door_component=component->identity();}
        replace_receipt(incomplete, restored);
        if(component)component->commit();
        throw std::runtime_error("setup_update_failed_previous_generation_restored");
    }
    // Receipt commit or acknowledgement failure must not roll files back under
    // an already-committed new receipt. The complete verified pair stays put;
    // an unreadable/uncommitted receipt continues to refuse ordinary startup.
    replace_receipt(incomplete, current);
    if(component)component->commit();
    if(retirement)retirement->retire();
    // The exact current transaction is eligible only after verified commit.
    for (std::size_t n = 0; n != previous.program_count; ++n) {
        const auto path = backup / programs[n];
        require(file_sha256(path) == previous.program_hashes[n], "setup_recovery_cleanup_identity_changed");
    }
    require(read_file(backup / L"receipt.recovery", maximum_receipt_bytes) == encode_receipt(previous),
        "setup_recovery_receipt_changed");
    for (std::size_t n = 0; n != previous.program_count; ++n)
        require(DeleteFileW((backup / programs[n]).c_str()) != FALSE, "setup_committed_recovery_cleanup_failed");
    require(DeleteFileW((backup / L"receipt.recovery").c_str()) != FALSE, "setup_committed_recovery_cleanup_failed");
    if (previous.program_count >= archive_program_count)
        require(RemoveDirectoryW((backup / L"Tools").c_str()) != FALSE, "setup_committed_recovery_cleanup_failed");
    require(RemoveDirectoryW(backup.c_str()) != FALSE, "setup_committed_recovery_cleanup_failed");
    const auto legacy=plan.root/L"Recovery";
    if(!absent(legacy)){
        auto held=open_object(legacy,true,GENERIC_READ|READ_CONTROL|DELETE,FILE_SHARE_READ);
        // Empty-only exact-object retirement. Old interrupted transactions
        // remain available at their original location for explicit recovery.
        if(exact_security(held.get(),structural.get())&&fs::is_empty(legacy)){
            FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
            require(SetFileInformationByHandle(held.get(),FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"setup_empty_legacy_recovery_cleanup_failed");
        }
    }
    finish_service_setup(plan,owner,channel,service_mode);
    commit_starter_door_copy(plan.root,owner);
    if (install_uninstall_entry) publish_uninstall_entry(current, ordinary_token);
    if(!clamav_package.empty()){
        // Commit the BBS before publishing the optional engine. An earlier
        // program rollback leaves the old engine and its data usable.
        bool updated{};
        try {
            platform::win32::provision_clamav_engine(plan.root,owner,current.id,clamav_package,
                [&]{channel.tick();},clamav_predecessor.value_or(std::wstring{}));
            updated=true;
        } catch(const std::exception&) {}
        channel.send(updated?"CLAMAV-UPDATED":"CLAMAV-UPGRADE-NOT-COMPLETED");
    }
    channel.send("COMMITTED");
}
} // namespace

void verify_payload() {
    (void)payload();
    require(sha256(resource(207))=="F4536F582FAE940C589078AEAA37F229B33EEF0A96D5375346E9E13588294F79",
        "setup_botgate_artwork_hash_mismatch");
    for (const auto& asset : stock_menu_assets)
        require(sha256(resource(asset.resource_id))==asset.sha256,"setup_stock_menu_payload_hash_mismatch");
    for (const auto& asset : starter_door_assets)
        require(sha256(resource(asset.resource_id))==asset.sha256,"setup_starter_payload_hash_mismatch");
    for(const auto& asset:sysop_doc_assets)
        require(sha256(resource(asset.resource_id))==asset.sha256,"setup_sysop_doc_payload_mismatch");
}
Plan inspect_plan(const Path& root, bool uninstall) {
    return inspect_plan(root,uninstall,{});
}
Plan inspect_plan(const Path& root, bool uninstall, const PlanningCallbacks& callbacks) {
    struct Clock {
        const PlanningCallbacks& callbacks; PlanningPhase phase; bool active{true};
        std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
        void emit(bool completed) noexcept {
            if(!callbacks.timing)return;
            try{callbacks.timing({phase,0,static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now()-start).count()),completed});}catch(...){}
        }
        void complete() noexcept{emit(true);active=false;}
        ~Clock(){if(active)emit(false);}
    };
    const auto checkpoint=[&]{
        bool proceed=true;
        try{if(callbacks.keep_going)proceed=callbacks.keep_going();}catch(...){throw PlanningObservationFailure();}
        if(!proceed)throw PlanningCancelled();
    };
    const auto status=[&](PlanningPhase phase){checkpoint();if(callbacks.status){try{callbacks.status(phase);}catch(...){}}};
    Clock total{callbacks,PlanningPhase::total};
    require(!elevated(), "setup_start_normally_not_as_administrator");
    status(PlanningPhase::embedded_payload);
    {Clock time{callbacks,PlanningPhase::embedded_payload};verify_payload();time.complete();}
    status(PlanningPhase::location);Clock location{callbacks,PlanningPhase::location};
    Plan plan; plan.root = canonical_root(root); plan.transaction = unique_id();
    auto token = process_token(); const auto ancestors = protect_ancestors(plan.root, token.get());
    location.complete();
    if (absent(plan.root)) {
        require(!uninstall, "setup_uninstall_installation_missing"); plan.operation = Operation::fresh;
    } else {
        auto root_handle = open_object(plan.root, true);
        plan.reviewed_root_id = file_id(root_handle.get()); plan.reviewed_volume = volume_serial(root_handle.get());
        // Construct observational callbacks outside the provenance fallback.
        // Allocation/control-observer errors must never classify the board.
        StructureVerificationCallbacks observation;
        observation.checkpoint=checkpoint;
        if(callbacks.timing)observation.timing=[&](const StructureVerificationTiming& event){
            auto phase=event.program?PlanningPhase::installed_program:PlanningPhase::installed_boundaries;
            switch(event.detail){
            case StructureVerificationTiming::Detail::open_security:phase=PlanningPhase::installed_open_security;break;
            case StructureVerificationTiming::Detail::native_identity:phase=PlanningPhase::installed_native_identity;break;
            case StructureVerificationTiming::Detail::file_read:phase=PlanningPhase::installed_read;break;
            case StructureVerificationTiming::Detail::sha:phase=PlanningPhase::installed_sha;break;
            default:break;
            }
            try{callbacks.timing({phase,event.role,event.microseconds,event.completed});}catch(...){}
        };
        try {
            status(PlanningPhase::receipt);Clock receipt_time{callbacks,PlanningPhase::receipt};
            auto receipt = read_receipt(plan.root, operator_sid());
            require(receipt.committed, "setup_incomplete_receipt");receipt_time.complete();
            status(PlanningPhase::installed_files);Clock installed_time{callbacks,PlanningPhase::installed_files};
            verify_structure(receipt,true,observation);installed_time.complete();
            // Keep the authenticated receipt's spelling through generation
            // replacement. Copied-board preparation may normalize path case;
            // native identity is already verified and receipt replacement
            // deliberately requires the exact same recorded root.
            plan.root = receipt.root;
            plan.previous = receipt; plan.operation = uninstall ? Operation::uninstall : Operation::maintenance;
        } catch (const PlanningCancelled&) { throw;
        } catch (const PlanningObservationFailure&) { throw;
        } catch (const std::exception&) {
            require(!uninstall, "setup_uninstall_provenance_mismatch_preserve_data");
            status(PlanningPhase::source_recognition);Clock recognized{callbacks,PlanningPhase::source_recognition};
            recognize_source(plan.root); plan.operation = Operation::rebuild;recognized.complete();
        }
    }
    status(PlanningPhase::details);Clock details{callbacks,PlanningPhase::details};
    if (plan.operation == Operation::maintenance || plan.operation == Operation::rebuild) {
        if(plan.previous && !plan.previous->ftn_layout)
            require(absent(plan.root/L"FTN"),"setup_untracked_ftn_directory_preserved_repair_required");
        // Refuse ambiguous dual trees before any preservation, staging or update.
        (void)display_source(plan.root);
        plan.backup_timestamp = archive_engine::current_timestamp();
        plan.backup = plan.root / L"Backup" / L"Upgrades" /
                      archive_engine::backup_filename(plan.backup_timestamp);
        // Archive availability is checked only after the explicit Yes/No
        // choice. A collision in an optional pool cannot block a declined ZIP.
        require(plan.backup.native().size() < 230 &&
                    (plan.operation != Operation::rebuild || (absent(plan.backup) &&
                    absent(plan.backup.native() + std::wstring(L".partial")))),
                "setup_backup_path_unavailable");
    }
    if (plan.operation == Operation::rebuild) {
        plan.staging = plan.root.native() + L"-migration-stage-" + plan.transaction;
        require(plan.staging.native().size() < 230 && absent(plan.staging),
                "setup_migration_staging_unavailable");
    }
    details.complete();checkpoint();total.complete();return plan;
}
Plan inspect_registered_uninstall(const std::wstring& id, const PlanningCallbacks& callbacks) {
    const auto selected = registered_installations({id}, true);
    require(selected.size() == 1 && selected.front().receipt.committed, "setup_uninstall_selected_record_unavailable");
    const auto& receipt = selected.front().receipt;
    if (callbacks.keep_going && !callbacks.keep_going()) throw PlanningCancelled();
    if (selected.front().available) {
        auto plan = inspect_plan(receipt.root, true, callbacks);
        require(plan.previous && plan.previous->id == receipt.id, "setup_uninstall_selected_record_changed");
        return plan;
    }
    const auto root = canonical_root(receipt.root);
    const auto token = process_token();
    const auto anchors = protect_ancestors(root, token.get());
    require_missing_installation(receipt);
    Plan plan;
    plan.root = root;
    plan.transaction = unique_id();
    plan.operation = Operation::uninstall_missing;
    plan.previous = receipt;
    return plan;
}
std::wstring summary(const Plan& plan) {
    SYSTEM_INFO system{};GetNativeSystemInfo(&system);
    const std::wstring door_note=system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_INTEL
        ?L"\n\nDOS Doors: Setup verifies SFXEXEC.DLL in Windows System32. Existing shared or in-use files are preserved unless a verified safe update is possible."
        :L"\n\nNative Doors are available. DOS Doors require a separately supplied compatible runtime; DOS execution on this Windows architecture is not yet qualified.";
    if (plan.operation == Operation::uninstall_missing)
        return L"Retire the record for this already-deleted SFX folder:\n" + plan.root.native() +
            L"\n\nConfirm only if you deleted this folder intentionally. If you moved it, disconnected a drive, or intend to restore it, choose No.";
    if (plan.operation == Operation::uninstall_full)
        return L"Permanently remove this installation and its entire folder:\n" + plan.root.native() +
            L"\n\nThis deletes your BBS database, callers, messages, files, settings, and backups inside the selected folder. Close all SFX programs first.";
    if (plan.operation == Operation::uninstall)
        return L"Remove the verified SFX programs and their installation receipt from:\n" + plan.root.native() +
            L"\n\nAll BBS data, backups and unknown files will be preserved. The shared DOS driver is removed only when introduced by SFX, unchanged and unused by any remaining installation. Close SFX before continuing.";
    if (plan.operation == Operation::maintenance)
        return L"Update/repair the verified installation at:\n" + plan.root.native() +
            (plan.backup.empty() ? L"\n\nOptional data backup skipped. Keep your own usable backup."
                : L"\n\nCreate and verify a mutable-data backup (not a full-system snapshot) at:\n" + plan.backup.native()) +
            L"\n\nExisting accounts and SSH identity will be retained. Temporary program recovery remains protected outside Bin. Close SFX before continuing."+door_note;
    if (plan.operation == Operation::rebuild)
        return L"This existing tree has no matching usable installation provenance. It will NOT be trusted or repaired in place.\n\n"
            L"Temporary source staging (never executed):\n" + plan.staging.native() +
            L"\n\nCreate and verify a preservation ZIP at:\n" + plan.backup.native() +
            L"\n\nCreate fresh programs at:\n" + plan.root.native() +
            L"\n\nImport data without elevation. Old programs are never run or installed. Work remains disposable. Close SFX before continuing."+door_note;
    return L"Install " SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE L" at:\n" + plan.root.native() +
        L"\n\nSetup creates protected programs and private data folders. Start the Control Panel afterward to configure the BBS and first Sysop."+door_note;
}
std::wstring completion_text(const Plan& plan,const Options& options){
    std::wstring result=options.start_menu||options.desktop
        ?L"Installation complete. Open the Control Panel from your selected shortcut."
        :L"Installation complete. Open the installed Control Panel:\n"+(plan.root/L"Bin"/L"SFXControlPanel.exe").native();
    if(!plan.backup.empty())result+=L"\nVerified backup: "+plan.backup.native();
    return result;
}
bool execute(const Plan& plan, const Options& options, HWND window, Progress output, Pump pump) {
    MaintenanceProgress reporting(std::move(output));
    const Progress progress=[&](const std::wstring& text){reporting.status(text);};
    require(!elevated(), "setup_coordinator_requires_ordinary_user"); verify_payload();
    require(!options.service_mode,"setup_mode_changes_use_configuration");
    const bool retain_existing_clamav=platform::win32::clamav_available()&&platform::win32::clamav_installed(plan.root);
    bool clamav_install_failed{};
    bool clamav_engine_verified{};
    std::wstring retained_clamav_message;
    auto clamav_package=options.clamav_package;std::unique_ptr<DownloadedClamAv> downloaded;
    if(options.automatic_clamav&&platform::win32::clamav_available()){
        require(!removing(plan.operation)&&clamav_package.empty(),"clamav_download_option_invalid");
        require(!retain_existing_clamav||plan.previous.has_value(),"clamav_component_receipt");
        const auto predecessor=retain_existing_clamav?platform::win32::confirmed_preparation_predecessor(*plan.previous):std::nullopt;
        require(!retain_existing_clamav||platform::win32::clamav_upgrade_required(plan.root,operator_sid(),
            plan.previous->id,predecessor.value_or(std::wstring{})),"clamav_engine_upgrade_not_selected");
        downloaded=DownloadedClamAv::fetch(progress,pump);clamav_package=downloaded->path();
    }
    if(!clamav_package.empty()){require(platform::win32::clamav_available(),"optional_component_unavailable_on_this_os");require(!removing(plan.operation),"clamav_uninstall_option_invalid");platform::win32::verify_clamav_package(clamav_package);}
    const auto who = operator_sid(); auto token = process_token(); const auto anchors = protect_ancestors(plan.root, token.get());
    Handle data_lock;
    if (!absent(plan.root / L"Data" / L"SFXRuntime.lock")) {
        auto root_handle = open_object(plan.root, true); auto data_handle = open_object(plan.root / L"Data", true);
        data_lock = open_object(plan.root / L"Data" / L"SFXRuntime.lock", false, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE);
    }
    const auto names = platform::win32::installation_object_names(plan.root);
    Handle guard(CreateMutexW(nullptr, FALSE, names.mutex_name.c_str()));
    const auto guard_error = GetLastError();
    require(guard && guard_error != ERROR_ALREADY_EXISTS && WaitForSingleObject(guard.get(), 0) == WAIT_OBJECT_0,
        "setup_close_control_panel_and_runtime_first");
    struct Unlock { HANDLE value; ~Unlock() { ReleaseMutex(value); } } unlock{guard.get()};
    // NTFS refuses a directory rename while a descendant file is open, even
    // with FILE_SHARE_DELETE. We have now excluded Runtime via its existing
    // installation-wide mutex; retain that guard (also held by the worker)
    // throughout preservation/import, but retire our source-file handle before
    // requesting the whole-tree rename. Maintenance/uninstall keep their lock.
    if (plan.operation == Operation::rebuild || plan.operation == Operation::uninstall_full) data_lock.reset();
    const auto pipe_name = L"\\\\.\\pipe\\SFX-Setup-" + unique_id();
    void* raw = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;GA;;;" + who +
        L")(A;;GA;;;SY)(A;;GA;;;BA)").c_str(), SDDL_REVISION_1, &raw, nullptr) != FALSE, "setup_pipe_security_failed");
    Descriptor sd(raw); SECURITY_ATTRIBUTES sa{sizeof(sa), sd.get(), FALSE};
    Channel channel; channel.pump = [&]{reporting.heartbeat();return !pump||pump();};
    channel.pipe.reset(CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        maximum_packet + 4, maximum_packet + 4, 5000, &sa));
    require(channel.pipe.get() != INVALID_HANDLE_VALUE, "setup_pipe_create_failed");
    const auto exe = module_path(); const auto args = L"--worker " + pipe_name + L" " + std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW launch{}; launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI; launch.hwnd = window;
    launch.lpVerb = L"runas"; launch.lpFile = exe.c_str(); launch.lpParameters = args.c_str(); launch.nShow = SW_HIDE;
    progress(L"Waiting for Windows administrator approval...");
    if (!ShellExecuteExW(&launch)) {
        if (GetLastError() == ERROR_CANCELLED) return false;
        throw std::runtime_error("setup_elevated_launch_failed");
    }
    channel.peer.reset(launch.hProcess);
    require(channel.peer != nullptr, "setup_worker_process_unavailable");
    try {
    const auto started = GetTickCount64();
    for (;;) {
        channel.tick(); if (ConnectNamedPipe(channel.pipe.get(), nullptr)) break;
        const auto error = GetLastError(); if (error == ERROR_PIPE_CONNECTED) break;
        require(error == ERROR_PIPE_LISTENING && GetTickCount64() - started < pipe_timeout_ms, "setup_worker_connect_failed"); Sleep(20);
    }
    ULONG client = 0;
    require(GetNamedPipeClientProcessId(channel.pipe.get(), &client) && client == GetProcessId(channel.peer.get()),
        "setup_worker_identity_mismatch");
    channel.send(operation_code(plan.operation) + "\n" + ascii(plan.root.native()) + "\n" +
        (plan.backup.empty() ? "-" : ascii(plan.backup.native())) + "\n" +
        (plan.staging.empty() ? "-" : ascii(plan.staging.native())) + "\n" +
        ascii(plan.transaction) + "\n" +
        std::to_string(plan.reviewed_root_id) + "\n" + std::to_string(plan.reviewed_volume));
    if (plan.previous) {
        const auto bytes = encode_receipt(*plan.previous);
        channel.send(std::string(bytes.begin(), bytes.end()));
    } else channel.send("NO-RECEIPT");
    channel.send(clamav_package.empty()?"NO-CLAMAV":ascii(clamav_package.native()));
    channel.send("SERVICE-KEEP");
    channel.send(options.install_uninstall_entry ? "REGISTER-UNINSTALL" : "SKIP-UNINSTALL-ENTRY");
    auto last_busy = GetTickCount64();
    const auto tick = [&] { channel.tick(); if (GetTickCount64() - last_busy > 1000) {
        channel.send("BUSY"); last_busy = GetTickCount64(); } };
    bool retained_history = false;
    for (;;) {
        const auto message = channel.receive();
        if(message=="SERVICE-PREPARE") {
            require(!removing(plan.operation),"service_conversion_operation_invalid");
            progress(L"Preparing existing protected credentials for LocalSystem; retaining original settings and SSH identity...");
            platform::win32::prepare_service_secret_bridge(plan.root,who);
            channel.send("READY");
        } else if (message == "UNINSTALL") {
            require(removing(plan.operation), "setup_uninstall_response_inconsistent");
            shortcut_changes(plan, options); channel.send("READY");
        } else if (message == "UNINSTALL-HISTORY-RETAINED") {
            require(removing(plan.operation), "setup_uninstall_history_response_inconsistent");
            retained_history = true;
        } else if (message.rfind("UNINSTALL-HISTORY ", 0) == 0) {
            require(removing(plan.operation) && plan.previous, "setup_uninstall_history_response_inconsistent");
            const auto records=registered_installations({wide(message.substr(18))},true);
            require(records.size()==1 && records.front().receipt.committed && !records.front().available &&
                records.front().receipt.volume==plan.previous->volume &&
                !_wcsicmp(records.front().receipt.root.c_str(),plan.root.c_str()),
                "setup_uninstall_history_receipt_changed");
            auto historical=plan;historical.previous=records.front().receipt;
            shortcut_changes(historical,options);channel.send("READY");
        } else if (message == "REMOVE-BOARD") {
            require(plan.operation == Operation::uninstall_full, "setup_uninstall_full_response_inconsistent");
            progress(L"Removing data and backups in the confirmed installation folder...");
        } else if (message == "INITIALIZE") {
            progress(L"Initializing private folders and importing preserved data (not elevated)...");
            ensure_backup_destinations(plan.root, who);
            if (plan.operation == Operation::rebuild) {
                progress(L"Creating and verifying the migration-source ZIP, then importing data without elevation...");
                create_content_backup(plan, who, tick,reporting);
                import_source(plan, who, tick);
            }
            verify_mutable(plan.root, who); ordinary_publication_probe(plan.root, who, plan.transaction);
            if (plan.operation == Operation::fresh) {
                // Fresh configuration only. Never replace an imported/operator list.
                const auto joker = plan.root / L"Config" / L"JOKER.DAT";
                auto config = open_object(plan.root / L"Config", true);
                auto content = mutable_descriptor(who);
                create_file(joker, {}, content.get());
                platform::win32::WindowsStorageDaclPolicy policy(who);
                require(policy.apply(joker, platform::win32::StorageDaclObjectKind::file).succeeded(),
                    "setup_joker_file_protection_failed");
                // Fresh only: maintenance and rebuild preserve custom content
                // and intentional absence. This mutable asset is not a program.
                const auto artwork=plan.root/L"Display"/L"bg-prelog.ans";
                create_file(artwork,resource(207),content.get());
                require(policy.apply(artwork,platform::win32::StorageDaclObjectKind::file).succeeded(),
                    "setup_botgate_artwork_protection_failed");
            }
            ensure_file_store_directories(plan.root,who);
            seed_missing_stock_menus(plan.root,who,progress);
            retire_other_board_folder(plan.root,who,tick);
            platform::win32::initialize_other_board_text(plan.root,who);
            const auto credentials=platform::win32::prepare_portable_connection_credentials(plan.root,who);
            if(!credentials.issues.empty())progress(L"Some saved connections could not be converted. Originals were preserved. Use BBS Health Check and correct only the affected connection settings; other Desktop operation remains available.");
            if(platform::win32::load_portable_board_key(plan.root)){
                const auto snapshot=platform::win32::refresh_portable_file_snapshot(plan.root,who);
                if(!snapshot.published||snapshot.deferred)progress(L"Some file identities could not be verified. Files and earlier proofs were preserved for review; BBS Health Check can diagnose affected paths.");
            }
            channel.send("READY");
        } else if (message == "BACKUP") {
            ensure_backup_destinations(plan.root, who);
            std::optional<platform::win32::BackupCollection> collection;
            if (!plan.backup.empty()) {
                progress(L"Creating and verifying the optional BBS mutable-data ZIP without elevation...");
                collection = platform::win32::open_backup_collection(plan.root, who, archive_engine::BackupKind::setup_upgrade);
                require(collection->directory == plan.backup.parent_path(), "setup_backup_collection_mismatch");
                create_content_backup(plan, who, tick,reporting);
            } else progress(L"Optional upgrade data backup declined; protected transaction recovery remains mandatory.");
            if(introducing_botgate_art(plan)) {
                const auto artwork=plan.root/L"Display"/L"bg-prelog.ans";
                if(absent(artwork)) {
                    auto content=mutable_descriptor(who);create_file(artwork,resource(207),content.get());
                    require(platform::win32::WindowsStorageDaclPolicy(who).apply(artwork,platform::win32::StorageDaclObjectKind::file).succeeded(),"setup_botgate_artwork_protection_failed");
                }
            }
            ensure_file_store_directories(plan.root,who);
            seed_missing_stock_menus(plan.root,who,progress);
            // Declining the optional ZIP does not authorize loss of migration
            // material. Preserve a verified copy; retire originals after commit.
            if (!absent(plan.root / L"conf")) {
                const auto retained = plan.root / L"Backup" / L"Upgrades" / (L"Source-conf-" + plan.transaction);
                make_private_directory(retained, who);
                copy_contents(plan.root / L"conf", retained, who, tick);
            }
            discard_work(plan.root / L"Work", tick);
            channel.send("READY");
        } else if(message=="VERIFY-FTN") {
            progress(L"Checking installation data and saved connections");
            require(plan.operation==Operation::maintenance,"setup_ftn_verification_response_inconsistent");
            verify_mutable(plan.root,who);
            ordinary_publication_probe(plan.root,who,plan.transaction);
            // Maintenance has its own ordinary-owner handshake and never
            // enters INITIALIZE. Convert readable legacy values before the
            // successful update can be copied away from their Windows keys.
            const auto credentials=platform::win32::prepare_portable_connection_credentials(plan.root,who);
            if(!credentials.issues.empty())progress(L"Some saved connections could not be converted. Originals were preserved. Use BBS Health Check and correct only the affected connection settings; other Desktop operation remains available.");
            if(platform::win32::load_portable_board_key(plan.root)){
                progress(L"Checking file identities");
                const auto snapshot=platform::win32::refresh_portable_file_snapshot(plan.root,who);
                if(!snapshot.published||snapshot.deferred)progress(L"Some file identities could not be verified. Files and earlier proofs were preserved for review; BBS Health Check can diagnose affected paths.");
            }
            channel.send("READY");
        } else if (message == "CLAMAV-VERIFIED") {
            clamav_engine_verified=true;
            retained_clamav_message = L"Existing Portable ClamAV verified. Definition updates remain with Runtime and SFXConfig.";
        } else if (message == "CLAMAV-REPAIRED") {
            clamav_engine_verified=true;
            retained_clamav_message = L"Portable ClamAV repaired for this installation. Check definition status in SFXConfig > Security > ClamAV.";
        } else if (message == "CLAMAV-CONFIG-PRESERVED") {
            clamav_engine_verified=true;
            retained_clamav_message = L"Portable ClamAV engine verified. Setup preserved the existing FreshClam configuration because it could not safely update it. Check SFXConfig > Security > ClamAV.";
        } else if (message == "CLAMAV-UNVERIFIED") {
            retained_clamav_message = L"Setup could not verify the existing Portable ClamAV receipt or engine. Existing component files were preserved. Check SFXConfig > Security > ClamAV.";
        } else if (message == "CLAMAV-UPGRADE-REQUIRED") {
            retained_clamav_message = L"Portable ClamAV requires an upgrade to 1.4.7. With Scan Uploads On, uploads are held until a required scan can finish. Use SFXConfig > Security > ClamAV to upgrade.";
        } else if (message == "CLAMAV-UPGRADE-NOT-COMPLETED") {
            clamav_install_failed=true;
            retained_clamav_message = L"Portable ClamAV 1.4.7 could not be installed. Existing ClamAV data and scanning settings were preserved. Check SFXConfig > Security > ClamAV before relying on scanning.";
        } else if (message == "CLAMAV-UPDATED") {
            clamav_engine_verified=true;
            retained_clamav_message = L"Portable ClamAV 1.4.7 verified. Existing definitions and scanning settings were preserved. Check definition readiness in SFXConfig > Security > ClamAV.";
        } else if(message=="PREPARE-PROGRAMS")progress(L"Preparing protected program recovery");
        else if(message=="REPLACE-PROGRAMS")progress(L"Replacing protected programs");
        else if(message=="VERIFY-PROGRAMS")progress(L"Verifying replaced programs and installation receipt");
        else if(message=="DOS-COMPONENT")progress(L"Verifying the shared SFX DOS driver in Windows System32");
        else if(message=="DOS-RETAINED")progress(L"Shared DOS driver retained: pre-existing, still shared, changed, or in use");
        else if(message=="ROLLBACK-PROGRAMS")progress(L"Update interrupted: restoring protected programs before finishing");
        else if (message == "COMMITTED" || message == "REMOVED") {
            // The peer is still alive until it acknowledges this receipt of completion.
            channel.send("ACK");
            require(WaitForSingleObject(channel.peer.get(), 5000) == WAIT_OBJECT_0, "setup_worker_exit_timeout");
            DWORD code = 0; require(GetExitCodeProcess(channel.peer.get(), &code) && !code, "setup_worker_exit_failed");
            if (message == "COMMITTED") {
                progress(L"Verifying the committed installation");
                verify_runtime_installation(plan.root, who);
                progress(L"Finishing installation settings");shortcut_changes(plan, options);
                if (plan.operation == Operation::maintenance) {
                    const auto cleanup_tick = [&] {reporting.heartbeat();require(!pump || pump(), "setup_post_commit_cleanup_cancelled_preserved"); };
                    try{
                    progress(L"Update committed: checking retained recovery files");
                    retire_committed_conf(plan, who, cleanup_tick);
                    platform::win32::initialize_other_board_text(plan.root, who);
                    if (!plan.backup.empty()) {
                        auto collection = platform::win32::open_backup_collection(plan.root, who,
                            archive_engine::BackupKind::setup_upgrade, {}, false);
                        platform::win32::finish_backup_collection(plan.root, who, collection,
                            plan.backup, archive_engine::BackupKind::setup_upgrade, 3,[&]{cleanup_tick();return false;},
                            [&](auto done,auto total){reporting.retention(done,total);},
                            [&](auto stage){reporting.finalization(stage);});
                    }
                    }catch(const std::exception&){
                        progress(L"Update committed and verified. Final recovery or backup checks did not finish. Keep the remaining recovery and backup files; review them before another update.");return true;
                    }
                }
                const auto complete_text=completion_text(plan,options);
                if(platform::win32::windows7_no_av()){
                    progress(complete_text);
                }else if(retain_existing_clamav||clamav_install_failed){
                    if(clamav_engine_verified){
                        try{
                            // Setup discards Work. Reopen only the private ClamAV
                            // role so status can inspect the retained definitions.
                            const auto work=platform::win32::FilePayloadDirectory::private_store(plan.root/L"Work",who);
                            const auto clamav_work=work.path()/L"ClamAV";
                            if(absent(clamav_work))create_directory(clamav_work,mutable_descriptor(who).get());
                            (void)platform::win32::FilePayloadDirectory::private_store(clamav_work,who);
                        }catch(const std::exception&){
                            retained_clamav_message+=L"\nThe private ClamAV work directory could not be verified. Check ClamAV status before relying on scanning.";
                        }
                    }
                    progress(std::wstring(complete_text) + (retained_clamav_message.empty() ? L"" : L"\n" + retained_clamav_message));
                }else if(platform::win32::clamav_installed(plan.root)){
                    progress(L"SFX installed. Checking optional ClamAV definitions (ordinary user, cancellable)...");
                    const auto receipt=read_receipt(plan.root,who);const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto update=platform::win32::update_clamav_definitions(plan.root,who,receipt.id,now,[&]{return channel.pump&&!channel.pump();},[&](std::uint64_t){if(channel.pump)channel.pump();});
                    if(!update.succeeded){std::wstring diagnostic;for(const auto ch:update.diagnostic)diagnostic+=static_cast<wchar_t>(static_cast<unsigned char>(ch));progress(L"ClamAV update status: "+diagnostic);}
                    progress(std::wstring(complete_text)+(update.succeeded?L"\nClamAV definitions ready; Runtime owns daily updates.":update.diagnostic=="clamav_update_already_attempted_today"?L"\nClamAV daily update was already attempted today. Runtime owns subsequent daily updates.":L"\nClamAV definitions are not ready. Caller uploads with scanning On will be held privately. Check Config\\ClamAV and Logs\\ClamAV; Runtime will attempt the next daily update."));
                }else progress(complete_text);
            } else if (retained_history) {
                progress(L"The selected SFX registration was removed. Setup kept an unverified record for the same path. "
                    L"Before installing there again, check the original drive or choose a different folder.\n" + plan.root.native());
            } else if (plan.operation == Operation::uninstall_full || plan.operation == Operation::uninstall_missing) {
                progress(L"Selected SFX registration removed. The folder is absent and available for a fresh install.\n" + plan.root.native());
            } else {
                progress(L"SFX programs removed. Your BBS data, backups and other retained files are here:\n" + plan.root.native() +
                    L"\n\nFor a fresh install at the same path, move the complete retained folder aside first, then reopen Setup.");
            }
            return true;
        } else throw std::runtime_error(message.rfind("FAIL ", 0) == 0 ? message.substr(5) : "setup_worker_response_invalid");
    }
    } catch (...) {
        // Closing the private connection asks the exact child to stop. Never
        // leave a healthy worker unobserved on an ordinary failure path.
        // A NOWAIT client can keep polling an empty pipe after the last server
        // handle closes. Explicitly disconnect the server instance first so a
        // worker awaiting READY observes the lost connection immediately.
        if(channel.pipe && channel.pipe.get()!=INVALID_HANDLE_VALUE)
            DisconnectNamedPipe(channel.pipe.get());
        channel.pipe.reset();
        require(WaitForSingleObject(channel.peer.get(), 5000) == WAIT_OBJECT_0,
            "setup_worker_still_exiting_keep_source_and_backups_do_not_launch_sfx");
        throw;
    }
}
int worker(const std::wstring& name, DWORD parent_process) noexcept {
    Channel channel;
    try {
        require(elevated() && name.rfind(L"\\\\.\\pipe\\SFX-Setup-", 0) == 0 &&
            transaction_id(name.substr(std::wstring(L"\\\\.\\pipe\\SFX-Setup-").size())), "setup_worker_arguments_invalid");
        channel.peer.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_process));
        require(channel.peer != nullptr, "setup_owner_process_unavailable");
        auto owner_token = process_token(parent_process); const auto owner = token_sid(owner_token.get());
        require(owner == operator_sid(), "setup_other_credential_elevation_refused");
        channel.pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        require(channel.pipe.get() != INVALID_HANDLE_VALUE, "setup_worker_pipe_unavailable");
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        require(SetNamedPipeHandleState(channel.pipe.get(), &mode, nullptr, nullptr) != FALSE, "setup_worker_pipe_mode_failed");
        ULONG server = 0; require(GetNamedPipeServerProcessId(channel.pipe.get(), &server) && server == parent_process,
            "setup_owner_connection_mismatch");
        const auto fields = split(channel.receive()); require(fields.size() == 7, "setup_plan_fields_invalid");
        const auto expected_receipt = channel.receive();
        const auto component=channel.receive();const Path clamav_package=component=="NO-CLAMAV"?Path{}:canonical_root(wide(component));
        const auto hosting=channel.receive();
        require(hosting=="SERVICE-KEEP","setup_mode_changes_use_configuration");
        const auto registration=channel.receive();
        require(registration=="REGISTER-UNINSTALL"||registration=="SKIP-UNINSTALL-ENTRY",
            "setup_uninstall_registration_option_invalid");
        const std::optional<bool> service_mode;
        if(!clamav_package.empty())platform::win32::verify_clamav_package(clamav_package);
        Plan plan; plan.root = canonical_root(wide(fields[1])); plan.transaction = wide(fields[4]);
        require(transaction_id(plan.transaction), "setup_transaction_invalid");
        if (fields[2] != "-") plan.backup = wide(fields[2]);
        if (fields[3] != "-") plan.staging = wide(fields[3]);
        const bool needs_backup = !plan.backup.empty();
        require((fields[0] != "rebuild" || needs_backup) &&
                    (!needs_backup || ((fields[0] == "maintenance" || fields[0] == "rebuild") &&
                                       plan.backup.parent_path() == plan.root / L"Backup" / L"Upgrades" &&
                                       plan.backup.native().size() < 230 &&
                                       backup_filename_valid(plan.backup) && absent(plan.backup) &&
                                       absent(plan.backup.native() + std::wstring(L".partial")))),
                "setup_backup_path_invalid");
        const auto staging_prefix = plan.root.native() + L"-migration-stage-";
        require((fields[0] == "rebuild") == !plan.staging.empty() &&
                    (fields[0] != "rebuild" ||
                     (plan.staging.native() == staging_prefix + plan.transaction &&
                      plan.staging.native().size() < 230 && absent(plan.staging))),
                "setup_migration_staging_path_invalid");
        const auto ancestors = protect_ancestors(plan.root, owner_token.get());
        if (fields[0] != "fresh" && fields[0] != "uninstall-missing") {
            auto selected = open_object(plan.root, true);
            require(fields[5] == std::to_string(file_id(selected.get())) &&
                fields[6] == std::to_string(volume_serial(selected.get())), "setup_reviewed_target_changed");
        }
        if (fields[0] == "fresh") {
            require(absent(plan.root) && fields[5] == "0" && fields[6] == "0" && expected_receipt == "NO-RECEIPT",
                "setup_fresh_target_now_exists"); plan.operation = Operation::fresh;
        }
        else if (fields[0] == "uninstall-missing") {
            plan.operation = Operation::uninstall_missing;
            const auto previous = decode_receipt(std::vector<std::uint8_t>(expected_receipt.begin(), expected_receipt.end()));
            const auto records = registered_installations({previous.id}, true);
            require(fields[5] == "0" && fields[6] == "0" && records.size() == 1 && !records.front().available &&
                previous.owner == owner && previous.root == plan.root &&
                encode_receipt(records.front().receipt) == encode_receipt(previous), "setup_uninstall_missing_receipt_changed");
            require_missing_installation(previous);
            plan.previous = previous;
        }
        else if (fields[0] == "rebuild") {
            auto target = open_object(plan.root, true); plan.operation = Operation::rebuild;
            // Never silently downgrade a now-valid protected installation into adoption.
            bool valid = false;
            try { auto r = read_receipt(plan.root, owner); verify_structure(r); valid = r.committed; } catch (...) {}
            require(!valid, "setup_target_changed_review_again");
            require(expected_receipt == "NO-RECEIPT", "setup_rebuild_plan_inconsistent");
        } else {
            require(fields[0] == "maintenance" || fields[0] == "uninstall" || fields[0] == "uninstall-full",
                "setup_operation_invalid");
            plan.operation = fields[0] == "uninstall-full" ? Operation::uninstall_full :
                fields[0] == "uninstall" ? Operation::uninstall : Operation::maintenance;
            plan.previous = read_receipt(plan.root, owner); verify_structure(*plan.previous);
            const auto bytes = encode_receipt(*plan.previous);
            require(expected_receipt == std::string(bytes.begin(), bytes.end()), "setup_reviewed_receipt_changed");
        }
        // Keep the existing installation-presence guard alive independently of
        // coordinator unwind/exit until this worker has finished all mutations.
        const auto names = platform::win32::installation_object_names(plan.root);
        Handle lifetime_guard(OpenMutexW(SYNCHRONIZE, FALSE, names.mutex_name.c_str()));
        require(lifetime_guard != nullptr, "setup_owner_lifetime_guard_missing");
        channel.tick();
        require(clamav_package.empty()||!removing(plan.operation),"clamav_uninstall_option_invalid");
        const auto com = (removing(plan.operation)||registration=="REGISTER-UNINSTALL") ?
            CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED) : S_FALSE;
        require(SUCCEEDED(com),"setup_uninstall_shell_initialization_failed");
        struct ComScope { bool active; ~ComScope(){if(active)CoUninitialize();} };
        const ComScope shell{removing(plan.operation)||registration=="REGISTER-UNINSTALL"};
        install_programs(plan, owner, channel, owner_token.get(), clamav_package, service_mode,
            registration=="REGISTER-UNINSTALL"&&!removing(plan.operation));
        require(channel.receive() == "ACK", "setup_completion_not_acknowledged"); return 0;
    } catch (const std::exception& error) {
        if (channel.pipe && channel.pipe.get() != INVALID_HANDLE_VALUE) {
            try { channel.send(std::string("FAIL ") + error.what()); } catch (...) {}
        }
        return 20;
    }
}
} // namespace sfx::apps::setup
