#include "sfx/platform/win32/starter_doors.hpp"
#include <algorithm>
#include <set>
#include <sstream>

namespace sfx::platform::win32::installation {
namespace {
constexpr auto marker=L"starter-doors.manifest";
constexpr auto pending=L"starter-install.pending";
bool missing(const Path& path) {
    const auto value=GetFileAttributesW(path.c_str());
    if(value!=INVALID_FILE_ATTRIBUTES) return false;
    const auto error=GetLastError();
    require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"starter_path_unavailable");
    return true;
}
void safe_member(std::wstring_view value) {
    const bool documentation=value.rfind(L"Docs/Doors/SFXGuess32/",0)==0||value.rfind(L"Docs/Doors/NewsWire-X-RSS/",0)==0;
    require(value.size()<256&&(value.rfind(L"Doors/SFXGuess32/",0)==0||
        value.rfind(L"Doors/NewsWire-X-RSS/",0)==0||documentation),"starter_member_invalid");
    if(documentation)require(Path(value).extension()!=L".exe"&&value.find(L"/config/")==value.npos,"starter_document_path_invalid");
    require(value.find(L"..") == value.npos&&value.find_first_of(L"\\:\r\n ") == value.npos,
        "starter_member_invalid");
    require(std::all_of(value.begin(),value.end(),[](wchar_t c){return c>32&&c<127;}),
        "starter_member_invalid");
}
void safe_directory(const Path& path) {
    auto object=open_object(path,true);
    (void)object; // open_object rejects reparse points and final-path aliases.
}
}
namespace {
void copy_starters(const Path& root,const std::wstring& owner,
    const std::string& id,const std::vector<StarterDoorFile>& files,bool resume) {
    require(files.size()==23&&id.size()==32,"starter_payload_invalid");
    auto structural=structural_descriptor(owner);auto content=mutable_descriptor(owner);
    safe_directory(root/L"Doors");
    begin_fresh_starter_copy(root,owner,id);
    // Never merge defaults with an existing component or adopt unknown content.
    for(const auto* name:{L"SFXGuess32",L"NewsWire-X-RSS"}) {
        const auto directory=root/L"Doors"/name;
        if(missing(directory))create_directory(directory,content.get());
        else require(resume,"starter_directory_collision_preserved");
        safe_directory(directory);
    }
    std::set<std::wstring> names;
    std::string manifest="SFX-STARTER-DOORS-2\n"+id+"\n";
    for(const auto& file:files) {
        safe_member(file.relative_path);
        require(names.insert(file.relative_path).second&&sha256(file.bytes)==file.sha256,
            "starter_payload_hash_mismatch");
        auto relative=Path(file.relative_path);relative.make_preferred();Path parent=root;
        for(const auto& leaf:relative.parent_path()) {
            parent/=leaf;
            if(missing(parent))create_directory(parent,content.get());
            safe_directory(parent);
        }
        const bool mutable_config=file.relative_path.find(L"/config/")!=std::wstring::npos;
        if(missing(root/relative))create_file(root/relative,file.bytes,mutable_config?content.get():structural.get());
        else {
            require(resume&&file_sha256(root/relative)==file.sha256,"starter_recovery_member_changed_preserved");
            if(!mutable_config)require(SetFileSecurityW((root/relative).c_str(),
                DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,structural.get()),"starter_recovery_security_failed");
        }
        require(file_sha256(root/relative)==file.sha256,"starter_installed_hash_mismatch");
        for(const auto c:file.relative_path)manifest+=static_cast<char>(c);
        manifest+=" "+file.sha256+"\n";
    }
    create_file(root/L"Bin"/marker,{manifest.begin(),manifest.end()},structural.get());
}
}
void begin_fresh_starter_copy(const Path& root,const std::wstring& owner,const std::string& id) {
    require(id.size()==32,"starter_copy_identity_invalid");
    auto object=open_object(root,true);auto sd=structural_descriptor(owner);
    const auto text="SFX-STARTER-COPY-1\n"+id+"\n"+std::to_string(volume_serial(object.get()))+
        "\n"+std::to_string(file_id(object.get()))+"\n";
    const auto path=root/L"Bin"/pending;
    if(missing(path))create_file(path,{text.begin(),text.end()},sd.get());
    else {
        auto file=open_object(path,false);const auto bytes=read_file(path,1024);
        require(exact_security(file.get(),sd.get())&&std::string(bytes.begin(),bytes.end())==text,
            "starter_copy_journal_collision_preserved");
    }
}
void install_fresh_starter_doors(const Path& root,const std::wstring& owner,
    const std::string& id,const std::vector<StarterDoorFile>& files) {
    copy_starters(root,owner,id,files,false);
}
void resume_owned_starter_doors(const Path& root,const Path& source,const std::wstring& owner,
    const std::string& id,const std::vector<StarterDoorFile>& files) {
    const auto journal=source/L"Bin"/pending;if(missing(journal))return;
    auto sd=structural_descriptor(owner);auto file=open_object(journal,false);
    require(exact_security(file.get(),sd.get()),"starter_recovery_journal_security");
    const auto bytes=read_file(journal,1024);std::istringstream input(std::string(bytes.begin(),bytes.end()));
    std::string magic,old_id,volume,identity,extra;
    require(std::getline(input,magic)&&magic=="SFX-STARTER-COPY-1"&&std::getline(input,old_id)&&old_id.size()==32&&
        std::getline(input,volume)&&std::getline(input,identity)&&!std::getline(input,extra),"starter_recovery_journal_invalid");
    // An explicitly uninstalled board has no identity and is never a fresh
    // component recovery candidate, even if older retained metadata remains.
    if(missing(source/L"Bin"/L"installation.id"))return;
    auto original=open_object(source,true);const auto installed_id=read_file(source/L"Bin"/L"installation.id",32);
    require(std::string(installed_id.begin(),installed_id.end())==old_id&&volume==std::to_string(volume_serial(original.get()))&&
        identity==std::to_string(file_id(original.get())),"starter_recovery_source_identity_changed");
    copy_starters(root,owner,id,files,true);
}
void commit_starter_door_copy(const Path& root,const std::wstring& owner) {
    const auto journal=root/L"Bin"/pending;if(missing(journal))return;
    require(verified_fresh_starter_doors(root,owner),"starter_copy_commit_unverified");
    require(DeleteFileW(journal.c_str()),"starter_copy_commit_pending_preserved");
}
bool verified_fresh_starter_doors(const Path& root,const std::wstring& owner) {
    const auto path=root/L"Bin"/marker;
    if(missing(path))return false;
    const auto receipt=read_receipt(root,owner);
    auto object=open_object(path,false);auto structural=structural_descriptor(owner);
    require(exact_security(object.get(),structural.get()),"starter_receipt_security_changed");
    const auto bytes=read_file(path,16384);
    std::istringstream input(std::string(bytes.begin(),bytes.end()));std::string line,id;
    require(std::getline(input,line)&&(line=="SFX-STARTER-DOORS-1"||line=="SFX-STARTER-DOORS-2")&&std::getline(input,id)&&
        std::wstring(id.begin(),id.end())==receipt.id,"starter_receipt_identity_mismatch");
    std::set<std::string> names;
    while(std::getline(input,line)) {
        const auto split=line.find(' ');
        require(split!=std::string::npos&&line.size()-split-1==64,"starter_receipt_invalid");
        const auto name=line.substr(0,split);const std::wstring relative(name.begin(),name.end());
        safe_member(relative);require(names.insert(name).second,"starter_receipt_duplicate");
        auto member=Path(relative);member.make_preferred();
        Path at=root;for(const auto& part:member.parent_path()){at/=part;safe_directory(at);}
        // Feed settings and documentation can be deliberately customized before
        // first run. Only the two actual programs bind launch eligibility.
        if(member.extension()==L".exe")
            require(file_sha256(root/member)==line.substr(split+1),"starter_component_changed");
    }
    require(names.size()==23&&names.count("Doors/SFXGuess32/SFXGuess32.exe")&&
        names.count("Doors/NewsWire-X-RSS/NewsWire-X-RSS.exe"),"starter_receipt_incomplete");
    return true;
}
}
