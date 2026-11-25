#include "sfx/platform/win32/file_scanner.hpp"
#include "sfx/platform/win32/clamav_resources.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <algorithm>
#include <array>

namespace sfx::platform::win32 {
namespace {
namespace app=application;
namespace ins=installation;
using ins::Handle; using ins::require; using Path=std::filesystem::path;
struct Child {
    Handle job, process, thread;
    bool* reaped{};
    ~Child(){if(process){if(WaitForSingleObject(process.get(),0)!=WAIT_OBJECT_0&&job)TerminateJobObject(job.get(),1);const bool done=WaitForSingleObject(process.get(),5000)==WAIT_OBJECT_0;if(reaped)*reaped=done;}}
};
struct Attributes {
    std::vector<std::uint8_t> bytes; LPPROC_THREAD_ATTRIBUTE_LIST value{};
    ~Attributes(){if(value)DeleteProcThreadAttributeList(value);}
};
struct Pipe {
    Handle read,write; std::string bytes; bool eof{};
    Pipe(){
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE r{},w{};
        require(CreatePipe(&r,&w,&sa,4096)!=FALSE,"scan_pipe_create");read.reset(r);write.reset(w);
        require(SetHandleInformation(read.get(),HANDLE_FLAG_INHERIT,0)!=FALSE,"scan_pipe_parent_private");
    }
    bool drain(std::size_t& total){
        while(!eof){
            DWORD available{};
            if(!PeekNamedPipe(read.get(),nullptr,0,nullptr,&available,nullptr)){
                if(GetLastError()==ERROR_BROKEN_PIPE){eof=true;return true;}
                throw std::runtime_error("scan_pipe_read_failed");
            }
            if(!available)return true;
            std::array<char,4096> buffer{};DWORD count{};
            require(ReadFile(read.get(),buffer.data(),std::min<DWORD>(available,static_cast<DWORD>(buffer.size())),&count,nullptr)&&count>0,"scan_pipe_read_failed");
            if(count>app::kFileScanOutputBytes-total)return false;
            bytes.append(buffer.data(),count);total+=count;
        }
        return true;
    }
};
std::wstring quoted(const Path& path){
    const auto& s=path.native();require(!s.empty()&&s.back()!=L'\\'&&s.find_first_of(L"\"\r\n")==std::wstring::npos,"scan_argument_invalid");
    return L"\""+s+L"\"";
}
std::string output_path(const Path& path){
    // Pinned Windows ClamAV writes OEM path text. Refuse unrepresentable roots
    // rather than matching a lossy alias to a different scanner result.
    BOOL changed{};const auto& p=path.native();
    const auto n=WideCharToMultiByte(CP_OEMCP,WC_NO_BEST_FIT_CHARS,p.data(),static_cast<int>(p.size()),nullptr,0,nullptr,&changed);
    require(n>0&&!changed,"scan_output_path_encoding");std::string out(static_cast<std::size_t>(n),'\0');
    require(WideCharToMultiByte(CP_OEMCP,WC_NO_BEST_FIT_CHARS,p.data(),static_cast<int>(p.size()),out.data(),n,nullptr,&changed)==n&&!changed,"scan_output_path_encoding");return out;
}
std::vector<Handle> pin_engine(const Path& root){
    require(ins::canonical_root(root)==root,"scan_engine_path");std::vector<Path> ancestors;
    for(auto p=root;!p.empty();){ancestors.push_back(p);const auto next=p.parent_path();if(next==p)break;p=next;}
    std::reverse(ancestors.begin(),ancestors.end());std::vector<Handle> pins;
    for(const auto& p:ancestors)pins.push_back(ins::open_object(p,true,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE));
    for(const auto& m:clamav_program_members()){
        const auto path=root/m.name;auto file=ins::open_object(path,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
        BY_HANDLE_FILE_INFORMATION info{};require(GetFileInformationByHandle(file.get(),&info)&&info.nNumberOfLinks==1&&
            ((static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow)==m.bytes,"scan_engine_member_identity");
        require(ins::file_sha256(path)==m.sha256,"scan_engine_hash");pins.push_back(std::move(file));
    }
    // No unpinned side-loaded program/library in the engine directory. Other
    // root documentation and license subdirectories are not executable inputs.
    WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((root/L"*").c_str(),&data);require(find!=INVALID_HANDLE_VALUE,"scan_engine_enumeration");
    bool safe=true;
    do{
        const Path name=data.cFileName;const auto extension=name.extension().native();
        if(!_wcsicmp(extension.c_str(),L".dll")||!_wcsicmp(extension.c_str(),L".exe")){
            if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))safe=false;
            if(std::none_of(clamav_program_members().begin(),clamav_program_members().end(),[&](const auto& m){return !_wcsicmp(m.name,data.cFileName);}))safe=false;
        }
    }while(FindNextFileW(find,&data));const auto error=GetLastError();FindClose(find);
    require(safe&&error==ERROR_NO_MORE_FILES,"scan_engine_unpinned_image");return pins;
}
bool same(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id&&a.bytes==b.bytes&&a.sha256==b.sha256;}
} // namespace

const std::vector<ClamAvPinnedMember>& legacy_clamav_program_members(){
    static const std::vector<ClamAvPinnedMember> members{
        {L"clamscan.exe",244224,"4CFC22365BC5B7D89651E5566D1FBF072438E4A1C5E0B8538640061646BC22E4"},
        {L"freshclam.exe",219648,"A4A2281EE225153655ECE2E67C7B6D77C44C7A5FC51E446D58AD12CB5BBF191F"},
        {L"concrt140.dll",258128,"0D999350DF074DDB4F5B376D86B7CD1AE278A41A1269C5C8C2C114C90C2C1C96"},
        {L"json-c.dll",48640,"6D959EF23E92B922373124157FA47FB6E844757F30BCC086CE7EE982E8ECA0C2"},
        {L"libbz2.dll",65024,"0EE284D4396FE7E650129057957A726A624C4EF5D74CDE8686147E395B304992"},
        {L"libclamav.dll",16644096,"18F82BE66C78CD9170271EAE58BDA9E5A26E30565CD40B784711BEC1A6738540"},
        {L"libclammspack.dll",93696,"705782ACDF4BBA2AF51536D77E8D5334ACBCB1243947FAA867CC8D43E9012270"},
        {L"libclamunrar_iface.dll",34304,"CB902B4C4439369BA85D55CED596E9062A03E8B2069FA07AD91C1FC91993FA30"},
        {L"libclamunrar.dll",388608,"5646ED9BFEC7E929A6C11B212B5973E22815834801A7386BEA54CFF7C08F8F0A"},
        {L"libcrypto-3.dll",4679168,"3AB8C282B33E3FE473984E49AFB9CC51DCFE3347B430A4BB7C7CA4CB78D0088C"},
        {L"libcurl.dll",725504,"00741CFF769681D6157403025E5EC3084D22627932C03350E794A4865867FA67"},
        {L"libfreshclam.dll",14927872,"38007328AF8E918FA549431841BC561191EE15BACE25DDA9423F676B14F7268D"},
        {L"libssh2.dll",255488,"C5A21C781B1719D8D8FB697F133D7AF145E77EC116FD7C0C97CCE14F849615BD"},
        {L"libssl-3.dll",1074176,"BCDE62EAB9E5441B56A4CBEC9D962284970C3D5AA6FFF5C0E5FEFBC65157884D"},
        {L"libxml2.dll",949760,"9A0156995E1C62E774100656CE1F068D5451A0BF589FD5DBF83CA340B5D7E206"},
        {L"msvcp140_1.dll",33872,"5ACA119C89BEF7BD8895A3E10A2953DF051A43BCBC53E544FE5A3428169AC20E"},
        {L"msvcp140_2.dll",250992,"B86D6734D9CE557938533EE40A22B0DAB9D9C8359B22B5DBC07CC090C5C4FACB"},
        {L"msvcp140_atomic_wait.dll",47184,"F8B73F92B98CA6E82911F20EA93A090F921E051062F04E7D80CB76123B07E35A"},
        {L"msvcp140_codecvt_ids.dll",30832,"8A03758A60938B37C9C00CD4E7810FC3CE9A3E0A2447C27D58626C69EA0BD3E9"},
        {L"msvcp140.dll",449616,"C8E7456F4AC9AA65EF3AD61A6DAF30EFEC9737344D173B2D6D2C16E752052A55"},
        {L"nghttp2.dll",132096,"78D19B8627A65ACB13C33B568FF74A040DED7665DD929E91CA627C79FAD6EBA2"},
        {L"pcre2-8.dll",359936,"1BBA123375AC9825D77F7A1AD4AEEFA4246D533BAB953B3919276F109C75D154"},
        {L"pdcurses.dll",206336,"162542A804199A7BDC212A6ABC549FB9E27C8C5F9D7799BC55F678FDFDAA9907"},
        {L"pthreadVC3.dll",68608,"CB6EC604F9C62E4B718C996D0973FF4327BEB3935C5F100D3ECD1C357DC5F052"},
        {L"vcruntime140.dll",91112,"C8E6089E6EFE9573AF55CF011C4E41B21235B2531F6C395FAAD53F410F22ACAA"}
    };return members;
}
const std::vector<ClamAvPinnedMember>& clamav_program_members(){
    static const std::vector<ClamAvPinnedMember> members{
        {L"clamscan.exe", 244224, "E37C6E471EDEECA4A93F19DC223A0F963BCA002544511FD29A0A8DD70BB9B73F"},
        {L"freshclam.exe", 219648, "14477EA8570C4204F904F8C9377AF55C93A4C59BD84004BA9C9B9B391E31EFFC"},
        {L"concrt140.dll", 258128, "0D999350DF074DDB4F5B376D86B7CD1AE278A41A1269C5C8C2C114C90C2C1C96"},
        {L"json-c.dll", 53760, "CABCB4467CBF4FDC4F5DC6B21A0C7FF77E3C9D0D15D03DC2B1217ACBEE6B1A63"},
        {L"libbz2.dll", 65024, "F5CC635213A8CC339BF2D60BC211ABCCD765FECFCB90940BBE641D8240E8A8B8"},
        {L"libclamav.dll", 16647680, "F8903736DE339697C925D5E83E630C9DC6D21FB7A5872AE5FBD927D55D25ACFE"},
        {L"libclammspack.dll", 93696, "D1CE63A8D1557476A930C185C822D76708E14ACE5C9B949D10D9393B983F3CF0"},
        {L"libclamunrar_iface.dll", 34304, "1FE7760EA98C7FADF215DBF905C31DFBB41F85C4A5A95264C671B9563DF99A18"},
        {L"libclamunrar.dll", 388608, "32E3513AA69C8560B91793FD02F7AAE771CDAA5FDF58BF5C3A971D440218AC17"},
        {L"libcrypto-3.dll", 4558848, "56FAEAA753D7D829801A1864A1175266D3510995AEA9D099BDD303D4F0B2B686"},
        {L"libcurl.dll", 718848, "98A254C4A9AC41B091D451C443C575E39DE34679B5D233DB8B875BA0C2CD6B7A"},
        {L"libfreshclam.dll", 14927872, "216F9C24BDFFC9957799929058160D8A38EB55BD931D5781DC33FFAA15530458"},
        {L"libssl-3.dll", 1075200, "82775069F187565D6079C45B79F4F4067F899E163214906A75D32C3C7D5B6FDA"},
        {L"libxml2.dll", 947712, "034BC344434B4D075CAE928038E18C9AC4C41C12C23F69DC45BE200555CD6216"},
        {L"msvcp140_1.dll", 33872, "5ACA119C89BEF7BD8895A3E10A2953DF051A43BCBC53E544FE5A3428169AC20E"},
        {L"msvcp140_2.dll", 250992, "B86D6734D9CE557938533EE40A22B0DAB9D9C8359B22B5DBC07CC090C5C4FACB"},
        {L"msvcp140_atomic_wait.dll", 47184, "F8B73F92B98CA6E82911F20EA93A090F921E051062F04E7D80CB76123B07E35A"},
        {L"msvcp140_codecvt_ids.dll", 30832, "8A03758A60938B37C9C00CD4E7810FC3CE9A3E0A2447C27D58626C69EA0BD3E9"},
        {L"msvcp140.dll", 449616, "C8E7456F4AC9AA65EF3AD61A6DAF30EFEC9737344D173B2D6D2C16E752052A55"},
        {L"nghttp2.dll", 133632, "A97F1EFBE3E65E5CFD07A711D99349E882D398739E6E88F65864F79319610B46"},
        {L"pcre2-8.dll", 361472, "141DE394901BAF5945606D4778A0806977C81AF1A492C9F370FB74E19AA642FA"},
        {L"pdcurses.dll", 206336, "74DC951C58516C239AADDC2C4D5DC8CC759190005870B3C9ADD31F26E84F835F"},
        {L"pthreadVC3.dll", 68608, "9B27E26E955471B9EF9DA83D11C4E2D4FB6FDB8D82435813DBB55640BE39190B"},
        {L"vcruntime140.dll", 91112, "C8E6089E6EFE9573AF55CF011C4E41B21235B2531F6C395FAAD53F410F22ACAA"},
    };
    return members;
}
WindowsFileScanner::WindowsFileScanner(Path engine,std::wstring owner):engine_(std::move(engine)),owner_(std::move(owner)){}
NativeFileScanResult WindowsFileScanner::scan(FilePayloadLease& input,const FilePayloadDirectory& definitions,
    const FilePayloadDirectory& scratch,std::uint64_t maximum,const std::function<bool()>& cancelled,
    const std::function<void(std::uint64_t)>& activity,std::uint32_t deadline)const{
    NativeFileScanResult result;const auto start=GetTickCount64();
    if(!clamav_available()){result.diagnostic="scan_host_unavailable";result.result={app::FileScanVerdict::error,{},"Malware scanning unavailable on this operating system."};return result;}
    try{
        require(maximum>0&&maximum<=app::kMaximumCallerFileBytes&&deadline>0&&deadline<=180000,"scan_policy_invalid");
        require(!owner_.empty()&&scratch.names(1).empty()&&scratch.path()!=definitions.path()&&scratch.path()!=engine_,"scan_scratch_invalid");
        if(cancelled&&cancelled()){result.result={app::FileScanVerdict::incomplete,{},"Scan cancelled before launch"};return result;}
        const auto original=input.identity();require(original.bytes<=maximum,"scan_input_exceeds_limit");
        const auto expanded=std::min<std::uint64_t>(maximum*4,app::kMaximumCallerFileBytes);
        auto resources=ClamAvResourceLease::acquire(scratch,owner_,expanded*2,cancelled,
            [&](std::uint64_t){if(activity)activity(GetTickCount64()-start);},deadline);
        // A received lease still owns WRITE/DELETE access after sealing. ClamAV
        // does not share that access when opening an input pathname. Keep the
        // original pinned and scan an exact private copy with a read-only lease.
        const auto poll_copy=[&](std::uint64_t,std::uint64_t){
            if(cancelled&&cancelled())throw std::runtime_error("scan_copy_cancelled");
            require(GetTickCount64()-start<deadline,"scan_copy_timeout");return true;
        };
        {
            auto copy=scratch.create("SFX-scan-input.bin",std::max<std::uint64_t>(1,original.bytes));
            for(std::uint64_t offset=0;offset<original.bytes;){poll_copy(0,0);const auto bytes=input.read(offset);require(!bytes.empty(),"scan_copy_read");copy.append(bytes.data(),bytes.size());offset+=bytes.size();}
            const auto copied=copy.identity(poll_copy);require(copied.bytes==original.bytes&&copied.sha256==original.sha256,"scan_copy_mismatch");
        }
        auto scan_input=scratch.read("SFX-scan-input.bin");
        const auto copy_identity=scan_input.identity(poll_copy);require(copy_identity.bytes==original.bytes&&copy_identity.sha256==original.sha256,"scan_copy_changed");
        const auto expected=output_path(scan_input.path());
        auto engine_pins=pin_engine(engine_);
        // FreshClam and scanning cannot mutate one another's active databases.
        // Official-db-only authenticates signatures inside the pinned engine.
        std::vector<FilePayloadLease> definition_pins;
        const auto names=definitions.names();
        for(const auto& name:names)require(name=="main.cvd"||name=="main.cld"||name=="daily.cvd"||name=="daily.cld"||name=="bytecode.cvd"||name=="bytecode.cld"||name=="freshclam.dat","scan_unexpected_database_file");
        for(const auto* base:{"main","daily","bytecode"}){
            std::string selected;
            for(const auto* suffix:{".cvd",".cld"}){const auto name=std::string(base)+suffix;if(std::find(names.begin(),names.end(),name)!=names.end()){require(selected.empty(),"scan_duplicate_database");selected=name;}}
            require(!selected.empty(),"scan_database_missing");definition_pins.push_back(definitions.read(selected));
        }
        Pipe output,error;SECURITY_ATTRIBUTES inherited{sizeof(inherited),nullptr,TRUE};
        Handle null_input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherited,OPEN_EXISTING,0,nullptr));
        auto lock_keeper=resources->inherit_lock(),budget_keeper=resources->inherit_budget();
        require(null_input.get()!=INVALID_HANDLE_VALUE,"scan_null_input");HANDLE handles[]{null_input.get(),output.write.get(),error.write.get(),lock_keeper.get(),budget_keeper.get()};
        Attributes attributes;SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);require(size>0&&size<65536,"scan_attributes_size");attributes.bytes.resize(size);attributes.value=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.bytes.data());
        require(InitializeProcThreadAttributeList(attributes.value,1,0,&size)&&UpdateProcThreadAttribute(attributes.value,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr),"scan_handle_allowlist");
        Child child;child.reaped=&result.process_reaped;child.job.reset(CreateJobObjectW(nullptr,nullptr));require(bool(child.job),"scan_job_create");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit=1;limits.ProcessMemoryLimit=static_cast<SIZE_T>(resources->child_memory_limit());
        require(SetInformationJobObject(child.job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"scan_job_limits");
        std::array<wchar_t,32768> windows{};const auto count=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));require(count>0&&count<windows.size(),"scan_windows_path");
        const Path system=Path(windows.data())/L"System32";
        std::wstring environment=L"PATH="+system.native()+L'\0'+L"SystemRoot="+windows.data()+L'\0'+L"TEMP="+scratch.path().native()+L'\0'+L"TMP="+scratch.path().native()+L'\0'+L'\0';
        const auto exe=engine_/L"clamscan.exe";
        std::wstring command=quoted(exe)+L" --stdout --allmatch --official-db-only=yes --fail-if-cvd-older-than=7 --database="+quoted(definitions.path())+L" --tempdir="+quoted(scratch.path())+
            L" --alert-encrypted=yes --alert-exceeds-max=yes --max-filesize="+std::to_wstring(maximum)+L" --max-scansize="+std::to_wstring(expanded)+
            L" --max-files=1000 --max-recursion=16 --max-scantime=120000 --follow-dir-symlinks=0 --follow-file-symlinks=0 -- "+quoted(scan_input.path());
        require(command.size()<30000,"scan_command_limit");
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;
        startup.StartupInfo.hStdInput=null_input.get();startup.StartupInfo.hStdOutput=output.write.get();startup.StartupInfo.hStdError=error.write.get();startup.lpAttributeList=attributes.value;
        PROCESS_INFORMATION process{};
        require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,environment.data(),engine_.c_str(),&startup.StartupInfo,&process),"scan_create_suspended");
        child.process.reset(process.hProcess);child.thread.reset(process.hThread);result.process_reaped=false;
        if(!AssignProcessToJobObject(child.job.get(),child.process.get())){TerminateProcess(child.process.get(),1);WaitForSingleObject(child.process.get(),2000);throw std::runtime_error("scan_job_assignment");}
        resources->bind_child(child.process.get());
        output.write.reset();error.write.reset();require(ResumeThread(child.thread.get())!=static_cast<DWORD>(-1),"scan_resume");child.thread.reset();
        app::FileScanEvidence evidence;evidence.started=true;evidence.expected_output_path=expected;std::size_t captured{};bool overflow{};std::uint64_t last_activity=UINT64_MAX;
        for(;;){
            if(!output.drain(captured)||!error.drain(captured)){overflow=true;break;}
            DWORD code{};require(GetExitCodeProcess(child.process.get(),&code)!=FALSE,"scan_exit_query");
            if(code!=STILL_ACTIVE){
                // The exit code can become visible before Windows has finished
                // process teardown and closed its inherited pipe writers. Wait
                // for actual termination before requiring both report EOFs.
                require(WaitForSingleObject(child.process.get(),5000)==WAIT_OBJECT_0,"scan_exit_wait");
                evidence.exited=true;evidence.exit_code=code;result.exit_code=code;
                overflow=!output.drain(captured)||!error.drain(captured);
                evidence.output_complete=!overflow&&output.eof&&error.eof;break;
            }
            const auto elapsed=GetTickCount64()-start;
            if(cancelled&&cancelled()){evidence.cancelled=true;break;}
            if(elapsed>=deadline){evidence.timed_out=true;break;}
            if(activity&&elapsed/500!=last_activity){last_activity=elapsed/500;activity(elapsed);}
            const auto waited=WaitForSingleObject(child.process.get(),25);require(waited==WAIT_TIMEOUT||waited==WAIT_OBJECT_0,"scan_wait");
        }
        if(!evidence.exited){require(TerminateJobObject(child.job.get(),1)!=FALSE,"scan_terminate");require(WaitForSingleObject(child.process.get(),2000)==WAIT_OBJECT_0,"scan_terminate_wait");}
        require(same(original,input.identity()),"scan_input_changed");
        require(same(copy_identity,scan_input.identity()),"scan_copy_changed");
        evidence.standard_output=output.bytes;evidence.standard_error=error.bytes;
        result.result=overflow?app::FileScanResult{app::FileScanVerdict::incomplete,{},"Scanner output exceeded limit"}:app::classify_clamav_scan(evidence);
    }catch(const std::exception& e){
        const std::string_view phase=e.what();result.diagnostic=phase.substr(0,5)=="scan_"&&phase.size()<80?std::string(phase):"scan_native_failure";
        result.result={app::FileScanVerdict::error,{},"Scanner unavailable or failed; retained privately"};
        if(phase=="scan_admission_cancelled")result.result={app::FileScanVerdict::incomplete,{},"Scan cancelled before launch"};
        if(phase=="scan_admission_busy")result.result={app::FileScanVerdict::timeout,{},"Scanner busy; retained privately"};
        if(phase=="scan_copy_cancelled")result.result={app::FileScanVerdict::incomplete,{},"Scan cancelled before launch"};
        if(phase=="scan_copy_timeout")result.result={app::FileScanVerdict::timeout,{},"Scan preparation timed out; retained privately"};
    }catch(...){result.diagnostic="scan_native_failure";result.result={app::FileScanVerdict::error,{},"Scanner unavailable or failed; retained privately"};}
    if(!result.process_reaped){result.diagnostic="scan_child_exit_unconfirmed";result.result={app::FileScanVerdict::error,{},"Scanner exit unconfirmed; retained privately"};}
    result.elapsed_milliseconds=GetTickCount64()-start;return result;
}
} // namespace sfx::platform::win32
