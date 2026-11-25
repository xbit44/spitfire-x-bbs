// Candidate-only ACL feasibility. All mutation is confined to a newly-created
// disposable fixture; no installed root, drive ACL, existing fixture or network.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
unsigned checks = 0;
void require(bool value, const std::string& label) {
    ++checks;
    if (!value) throw std::runtime_error(label + " (Win32 " + std::to_string(GetLastError()) + ")");
}
struct Close { void operator()(void* p) const { if (p && p != INVALID_HANDLE_VALUE) CloseHandle(p); } };
struct Free { void operator()(void* p) const { if (p) LocalFree(p); } };
using Handle = std::unique_ptr<void, Close>;
using Local = std::unique_ptr<void, Free>;

std::wstring identity() {
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "read token");
    Handle token(raw); DWORD size = 0; TOKEN_ELEVATION elevation{};
    require(GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size) &&
        elevation.TokenIsElevated == 0, "ordinary non-elevated test token required");
    GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    require(GetTokenInformation(raw, TokenUser, data.data(), size, &size), "read user");
    wchar_t* text = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text), "encode SID");
    Local own(text); return text;
}

Local descriptor(const std::wstring& user, bool boundary) {
    // Numeric masks avoid generic mapping surprises. No DELETE, WRITE_DAC,
    // WRITE_OWNER, WRITE_ATTRIBUTES, WRITE_EA or DELETE_CHILD on the boundary.
    std::wstring sddl = L"O:" + user + L"D:P(A;;RC;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)(A;;" +
        (boundary ? std::wstring(L"0x1200af") : std::wstring(L"0x1200a9")) + L";;;" + user + L")";
    // Existing private-content permissions apply to children, not the boundary.
    if (boundary) sddl += L"(A;OICIIO;FA;;;SY)(A;OICIIO;FA;;;BA)(A;OICIIO;0x1301ff;;;" + user + L")";
    PSECURITY_DESCRIPTOR result = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        &result, nullptr), "build candidate descriptor");
    return Local(result);
}

bool exact(const fs::path& path, void* expected) {
    auto native = path.native(); PSID owner = nullptr, want_owner = nullptr;
    PACL acl = nullptr, want_acl = nullptr; PSECURITY_DESCRIPTOR raw = nullptr;
    if (GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &raw)) return false;
    Local sd(raw); BOOL d = FALSE, present = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (!GetSecurityDescriptorOwner(expected, &want_owner, &d) || !EqualSid(owner, want_owner) ||
        !GetSecurityDescriptorDacl(expected, &present, &want_acl, &d) || !present || !acl || !want_acl ||
        !GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PROTECTED)) return false;
    if (acl->AceCount != want_acl->AceCount) return false;
    std::vector<bool> consumed(acl->AceCount);
    for (unsigned i=0; i<want_acl->AceCount; ++i) {
        void* want = nullptr; if (!GetAce(want_acl,i,&want)) return false;
        bool found=false;
        for (unsigned j=0; j<acl->AceCount; ++j) {
            void* entry = nullptr; if (!GetAce(acl,j,&entry)) return false;
            const auto* header = static_cast<ACE_HEADER*>(want);
            if (!consumed[j] && static_cast<ACE_HEADER*>(entry)->AceSize == header->AceSize &&
                !std::memcmp(want,entry,header->AceSize)) { consumed[j]=true; found=true; break; }
        }
        if (!found) return false;
    }
    return true;
}

void seal(const fs::path& path, void* sd) {
    auto native = path.native(); PACL acl = nullptr; PSID owner = nullptr; BOOL d=FALSE,p=FALSE;
    require(GetSecurityDescriptorOwner(sd,&owner,&d) && GetSecurityDescriptorDacl(sd,&p,&acl,&d) && p,
        "read candidate descriptor");
    require(SetNamedSecurityInfoW(native.data(),SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
        owner,nullptr,acl,nullptr)==ERROR_SUCCESS,"seal owned disposable object");
    require(exact(path,sd),"verify exact candidate owner and DACL");
}

void make_directory(const fs::path& path) {
    require(CreateDirectoryW(path.c_str(),nullptr),"create fresh disposable directory");
}
void make_file(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(file.get()!=INVALID_HANDLE_VALUE,"create owned content");
    DWORD done=0; const char content[]="disposable original";
    require(WriteFile(file.get(),content,sizeof(content),&done,nullptr) && done==sizeof(content),"write owned content");
}

std::vector<BYTE> ea_value() {
    const std::string name="SFX.BOUNDARY";
    std::vector<BYTE> ea(8+name.size()+1+4);
    ea[5]=static_cast<BYTE>(name.size()); ea[6]=4;
    std::memcpy(ea.data()+8,name.c_str(),name.size()+1);
    const BYTE value[4]{'S','F','X',1};
    std::memcpy(ea.data()+8+name.size()+1,value,4);
    return ea;
}

void write_ea(const fs::path& path, DWORD access, bool expected) {
    Handle h(CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(h.get()!=INVALID_HANDLE_VALUE,"open candidate EA fixture handle");
    auto ea=ea_value();
    constexpr auto header_size=offsetof(WIN32_STREAM_ID,cStreamName);
    WIN32_STREAM_ID header{}; header.dwStreamId=BACKUP_EA_DATA; header.Size.QuadPart=ea.size();
    std::vector<BYTE> bytes(header_size+ea.size());
    std::memcpy(bytes.data(),&header,header_size); std::memcpy(bytes.data()+header_size,ea.data(),ea.size());
    void* context=nullptr; DWORD done=0;
    const BOOL result=BackupWrite(h.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&done,FALSE,FALSE,&context);
    const DWORD error=result?ERROR_SUCCESS:GetLastError();
    BackupWrite(h.get(),nullptr,0,&done,TRUE,FALSE,&context);
    std::cout << "EA write, mask " << access << ": " << (result?"SUCCEEDED":"DENIED")
        << " (Win32 " << error << ")\n" << std::flush;
    require(expected?result!=FALSE:!result&&error==ERROR_ACCESS_DENIED,"EA write/deny gate");
}

void verify_ea(const fs::path& path) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(h.get()!=INVALID_HANDLE_VALUE,"open boundary EA read");
    void* context=nullptr; DWORD done=0; bool found=false,valid=true;
    for(unsigned i=0;i<16;++i) {
        WIN32_STREAM_ID header{};
        if(!BackupRead(h.get(),reinterpret_cast<BYTE*>(&header),offsetof(WIN32_STREAM_ID,cStreamName),
            &done,FALSE,FALSE,&context)) { valid=false; break; }
        if(!done) break;
        if(done!=offsetof(WIN32_STREAM_ID,cStreamName) || header.dwStreamNameSize ||
            header.Size.QuadPart<0 || header.Size.QuadPart>16384) { valid=false; break; }
        std::vector<BYTE> data(static_cast<std::size_t>(header.Size.QuadPart));
        if(!data.empty() && (!BackupRead(h.get(),data.data(),static_cast<DWORD>(data.size()),
            &done,FALSE,FALSE,&context)||done!=data.size())) { valid=false; break; }
        if(header.dwStreamId==BACKUP_EA_DATA) { valid=!found&&data==ea_value(); found=true; }
        if(!valid) break;
    }
    BackupRead(h.get(),nullptr,0,&done,TRUE,FALSE,&context);
    require(valid&&found,"exact immutable EA value");
}
void denied_access(const fs::path& path, DWORD desired, const char* label) {
    Handle h(CreateFileW(path.c_str(),desired,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    const DWORD error = GetLastError();
    require(h.get()==INVALID_HANDLE_VALUE && error==ERROR_ACCESS_DENIED,label);
}

void verify_original(const fs::path& path) {
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(h.get()!=INVALID_HANDLE_VALUE,"read protected original");
    const char expected[]="disposable original"; char actual[sizeof(expected)+1]{}; DWORD done=0;
    require(ReadFile(h.get(),actual,sizeof(actual),&done,nullptr) && done==sizeof(expected) &&
        !std::memcmp(expected,actual,sizeof(expected)),"replacement attempts preserved original bytes");
}

void replacement_checks(const fs::path& source, const fs::path& protected_file, void* sd) {
    require(!CopyFileW(source.c_str(),protected_file.c_str(),FALSE) && GetLastError()==ERROR_ACCESS_DENIED,
        "copy-over protected file denied");
    const BOOL moved=MoveFileExW(source.c_str(),protected_file.c_str(),MOVEFILE_REPLACE_EXISTING);
    const DWORD move_error=GetLastError();
    require(!moved && (move_error==ERROR_ACCESS_DENIED || move_error==ERROR_SHARING_VIOLATION),
        "parent-mediated file replacement denied");
    require(!ReplaceFileW(protected_file.c_str(),source.c_str(),nullptr,0,nullptr,nullptr),
        "atomic protected-file replacement denied");
    verify_original(source); verify_original(protected_file);
    require(exact(protected_file,sd),"replacement attempts preserved exact security");
}

void boundary_checks(const fs::path& path, void* sd) {
    for (DWORD access : std::array<DWORD,6>{WRITE_DAC,WRITE_OWNER,DELETE,FILE_DELETE_CHILD,FILE_WRITE_ATTRIBUTES,FILE_WRITE_EA})
        denied_access(path,access,"structural privilege denied");
    auto native=path.native(); PACL acl=nullptr; BOOL p=FALSE,d=FALSE; PSID owner=nullptr;
    require(GetSecurityDescriptorDacl(sd,&p,&acl,&d) && GetSecurityDescriptorOwner(sd,&owner,&d),"parse attempted rewrite");
    require(SetNamedSecurityInfoW(native.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,
        nullptr,nullptr,acl,nullptr)==ERROR_ACCESS_DENIED,"permission rewrite denied even to same owner");
    require(SetNamedSecurityInfoW(native.data(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,
        owner,nullptr,nullptr,nullptr)==ERROR_ACCESS_DENIED,"ownership rewrite denied");
    const fs::path moved=path.native()+L"-renamed";
    require(!MoveFileExW(path.c_str(),moved.c_str(),0) && GetLastError()==ERROR_ACCESS_DENIED,"real structural rename denied");
    require(exact(path,sd),"failed attacks preserve exact structure");
}

void junction_attempt(const fs::path& path, const fs::path& target, DWORD access, bool expect_denied) {
    Handle h(CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if (h.get()==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        std::cout << "Junction handle open denied (Win32 " << error << ")\n";
        require(expect_denied && (error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION),"junction open denied"); return;
    }
    const std::wstring substitute=L"\\??\\"+target.native();
    const std::wstring print=target.native();
    struct Header { ULONG tag; USHORT length,reserved,suboffset,sublength,printoffset,printlength; };
    static_assert(sizeof(Header)==16,"junction buffer header");
    const auto subbytes=static_cast<USHORT>(substitute.size()*sizeof(wchar_t));
    const auto printbytes=static_cast<USHORT>(print.size()*sizeof(wchar_t));
    std::vector<BYTE> buffer(sizeof(Header)+subbytes+printbytes+4);
    auto* header=reinterpret_cast<Header*>(buffer.data());
    *header={IO_REPARSE_TAG_MOUNT_POINT,static_cast<USHORT>(buffer.size()-8),0,0,subbytes,
        static_cast<USHORT>(subbytes+2),printbytes};
    std::memcpy(buffer.data()+sizeof(Header),substitute.c_str(),subbytes+2);
    std::memcpy(buffer.data()+sizeof(Header)+subbytes+2,print.c_str(),printbytes+2);
    DWORD bytes=0;
    const BOOL set=DeviceIoControl(h.get(),FSCTL_SET_REPARSE_POINT,buffer.data(),
        static_cast<DWORD>(buffer.size()),nullptr,0,&bytes,nullptr);
    const DWORD error=set?ERROR_SUCCESS:GetLastError();
    std::cout << "Junction conversion, requested mask " << access << ": " << (set?"SUCCEEDED":"DENIED")
        << " (Win32 " << error << ")\n" << std::flush;
    require(expect_denied ? !set && (error==ERROR_ACCESS_DENIED || error==ERROR_DIR_NOT_EMPTY || error==ERROR_EA_ACCESS_DENIED) : set!=FALSE,
        expect_denied?"STRUCTURAL REPARSE CONVERSION MUST BE DENIED":"positive junction fixture must genuinely work");
}

void opaque_reparse_attempt(const fs::path& path, bool expect_denied, bool directory_bit=false) {
    // A well-formed private GUID reparse record needs no filter/driver/network.
    // Do not follow it: this asks whether the boundary can be marked at all.
    Handle h(CreateFileW(path.c_str(),FILE_ADD_FILE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(h.get()==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        std::cout << "Private GUID handle open denied (Win32 " << error << ")\n";
        require(expect_denied && (error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION),
            "content-create access denied for opaque reparse test"); return;
    }
    require(true,"content-create access for opaque reparse test");
    struct Record { ULONG tag; USHORT length,reserved; GUID owner; BYTE payload[4]; };
    Record record{0x00000042,4,0,{0x79d0c6a1,0x2f08,0x41da,{0xab,0x66,0x41,0x22,0x68,0x82,0x03,0x51}},{1,2,3,4}};
    if(directory_bit) record.tag |= 0x10000000;
    DWORD returned=0;
    const BOOL set=DeviceIoControl(h.get(),FSCTL_SET_REPARSE_POINT,&record,sizeof(record),nullptr,0,&returned,nullptr);
    const DWORD error=set?ERROR_SUCCESS:GetLastError();
    std::cout << "Private GUID reparse on directory (D=" << directory_bit << "): " << (set?"SUCCEEDED":"DENIED")
        << " (Win32 " << error << ")\n" << std::flush;
    if(set) {
        BYTE readback[16384]{}; DWORD count=0;
        require(DeviceIoControl(h.get(),FSCTL_GET_REPARSE_POINT,nullptr,0,readback,sizeof(readback),&count,nullptr)
            && count==sizeof(record) && !std::memcmp(readback,&record,sizeof(record)),
            "native reparse record readback exactly matches");
        BY_HANDLE_FILE_INFORMATION info{};
        require(GetFileInformationByHandle(h.get(),&info) && (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),
            "native handle confirms reparse attribute actually set");
        std::cout << "Verified native reparse tag 0x" << std::hex << record.tag << std::dec << '\n';
    }
    require(expect_denied ? !set && (error==ERROR_ACCESS_DENIED || error==ERROR_DIR_NOT_EMPTY || error==ERROR_EA_ACCESS_DENIED) : set!=FALSE,
        expect_denied?"STRUCTURAL PRIVATE REPARSE MUST BE DENIED":"positive private reparse must genuinely work");
}

int main(int argc, char** argv) {
    try {
        const bool lifetime_pin = argc==2 && std::string(argv[1])=="--lifetime-pin-control";
        const bool guard_file = lifetime_pin || (argc==2 && std::string(argv[1])=="--guard-file");
        const bool guard_ea = argc==2 && std::string(argv[1])=="--guard-ea";
        require(argc==1 || guard_file || guard_ea,"only optional guard candidate selector allowed");
        std::cout << "Candidate: " << (lifetime_pin?"LIFETIME-ONLY control; not permanent structural protection":guard_ea?"protected structural EA":guard_file?"protected structural guard file":"ACL only") << '\n';
        const auto cwd=fs::current_path();
        require(fs::is_regular_file(cwd/"test/platform/consolidation_structural_acl_feasibility.cpp"),"run from checkout");
        const auto parent=cwd/"build/consolidation-security-probe";
        for(auto p=parent;;p=p.parent_path()) {
            const DWORD a=GetFileAttributesW(p.c_str());
            require(a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_REPARSE_POINT),"no reparse fixture parent");
            if(p.parent_path()==p) break;
        }
        const auto user=identity(); auto structural=descriptor(user,false); auto boundary=descriptor(user,true);
        const auto guard=parent/("candidate-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        std::wcout << L"Fixture: " << guard.native() << L"\n";
        const auto root=guard/"SFX"; make_directory(guard); make_directory(root); make_directory(root/"Bin");
        const std::array<const wchar_t*,7> leaves{L"Config",L"Data",L"Displays",L"Backups",L"Logs",L"Doors",L"Work"};
        for(auto leaf:leaves) make_directory(root/leaf);
        if(guard_file) for(auto leaf:leaves) {
            make_file(root/leaf/L"SFX.boundary"); seal(root/leaf/L"SFX.boundary",structural.get());
        }
        if(guard_ea) for(auto leaf:leaves) { write_ea(root/leaf,FILE_WRITE_EA,true); verify_ea(root/leaf); }
        make_file(root/"Bin/SFXControlPanel.exe"); make_file(root/"Bin/SFXRuntime.exe");
        make_directory(guard/"target"); make_directory(guard/"junction-positive");
        junction_attempt(guard/"junction-positive",guard/"target",GENERIC_WRITE,false);
        make_directory(guard/"opaque-positive");
        opaque_reparse_attempt(guard/"opaque-positive",false);
        make_directory(guard/"opaque-directory-positive");
        opaque_reparse_attempt(guard/"opaque-directory-positive",false,true);
        // This fixture's guard controls the immediate parent. Its workspace
        // ancestors are NOT asserted to be a secure canonical installation.
        for(auto leaf:leaves) seal(root/leaf,boundary.get());
        seal(root/"Bin/SFXControlPanel.exe",structural.get()); seal(root/"Bin/SFXRuntime.exe",structural.get());
        seal(root/"Bin",structural.get()); seal(root,structural.get()); seal(guard,structural.get());
        boundary_checks(root,structural.get()); boundary_checks(root/"Bin",structural.get());
        denied_access(root,FILE_ADD_FILE,"root file creation denied");
        denied_access(root,FILE_ADD_SUBDIRECTORY,"root boundary creation denied");
        denied_access(root/"Bin",FILE_ADD_FILE,"Bin file creation denied");
        denied_access(root/"Bin",FILE_ADD_SUBDIRECTORY,"Bin directory creation denied");
        make_file(root/"Work/replacement.tmp");
        for(auto file:{"SFXControlPanel.exe","SFXRuntime.exe"}) {
            const auto path=root/"Bin"/file;
            for(DWORD mask:std::array<DWORD,4>{GENERIC_WRITE,WRITE_DAC,WRITE_OWNER,DELETE}) denied_access(path,mask,"binary mutation access denied");
            require(!DeleteFileW(path.c_str()) && GetLastError()==ERROR_ACCESS_DENIED,"binary delete denied");
            require(!MoveFileExW(path.c_str(),(root/fs::path(file)).c_str(),0) && GetLastError()==ERROR_ACCESS_DENIED,"binary rename denied");
            require(exact(path,structural.get()),"binary exact ACL unchanged");
            replacement_checks(root/"Work/replacement.tmp",path,structural.get());
        }
        for(auto leaf:leaves) {
            const auto path=root/leaf;
            Handle pin;
            if(lifetime_pin) {
                pin.reset(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                require(pin.get()!=INVALID_HANDLE_VALUE,"lifetime-only read pin acquired");
            }
            boundary_checks(path,boundary.get());
            make_file(path/L"owned.tmp");
            require(DeleteFileW((path/L"owned.tmp").c_str()),"delete owned file");
            make_directory(path/L"owned-job"); make_file(path/L"owned-job/owned.tmp");
            require(DeleteFileW((path/L"owned-job/owned.tmp").c_str()),"delete owned job file");
            require(RemoveDirectoryW((path/L"owned-job").c_str()),"delete empty owned job");
            require(!RemoveDirectoryW(path.c_str()) && GetLastError()==ERROR_ACCESS_DENIED,"boundary deletion denied");
            if(guard_file) {
                for(DWORD mask:std::array<DWORD,4>{GENERIC_WRITE,WRITE_DAC,WRITE_OWNER,DELETE})
                    denied_access(path/L"SFX.boundary",mask,"structural guard mutation denied");
                require(!DeleteFileW((path/L"SFX.boundary").c_str()) && GetLastError()==ERROR_ACCESS_DENIED,"guard delete denied");
                require(!MoveFileExW((path/L"SFX.boundary").c_str(),(path/L"guard-moved").c_str(),0) &&
                    GetLastError()==ERROR_ACCESS_DENIED,"guard rename denied");
                replacement_checks(root/"Work/replacement.tmp",path/L"SFX.boundary",structural.get());
            }
            for(DWORD mask:{DWORD(0),DWORD(FILE_ADD_FILE),DWORD(FILE_ADD_SUBDIRECTORY),DWORD(GENERIC_WRITE)})
                junction_attempt(path,guard/"target",mask,true);
            opaque_reparse_attempt(path,true);
            opaque_reparse_attempt(path,true,true);
            if(guard_ea) { write_ea(path,FILE_ADD_FILE,false); verify_ea(path); }
            require(!(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"boundary remains real directory");
            require(exact(path,boundary.get()),"content operations preserve boundary owner/ACL");
        }
        require(DeleteFileW((root/"Work/replacement.tmp").c_str()),"owned replacement source cleanup");
        if(lifetime_pin) {
            std::cout << "Pins released (shutdown control): the SAME protected nonempty directory remains vulnerable.\n";
            opaque_reparse_attempt(root/L"Config",false,true);
            std::cout << "LIFETIME-ONLY CONTROL PASS: " << checks << " checks. Permanent boundary NOT qualified.\n";
            return 0;
        }
        std::cout << "CANDIDATE RIGHTS PASS: " << checks << " checks. No bootstrap/UAC/Win7 qualification claim.\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "CANDIDATE NOT QUALIFIED after " << checks << " checks: " << e.what()
            << "\nDisposable fixture retained; no installed-system change.\n";
        return 1;
    }
}
