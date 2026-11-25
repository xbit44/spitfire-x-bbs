#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace sfx::apps::runtime {
using WorkProblemObserver=std::function<void(std::string,std::string)>;
// All handles belong to this supervised job. No process discovery or foreign
// process termination. A reaped parent alone does not prove descendants stopped.
inline bool stop_owned_process_job(HANDLE job,HANDLE process,bool assigned)noexcept{
    if(process&&WaitForSingleObject(process,0)!=WAIT_OBJECT_0){
        const bool requested=assigned&&job?TerminateJobObject(job,1)!=FALSE:TerminateProcess(process,1)!=FALSE;
        if(!requested&&WaitForSingleObject(process,0)!=WAIT_OBJECT_0)return false;
        if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0)return false;
    }
    if(assigned&&job){
        const auto until=GetTickCount64()+5000;bool requested{};
        for(;;){JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&info,sizeof(info),nullptr))return false;
            if(!info.ActiveProcesses)return true;
            if(!requested){if(!TerminateJobObject(job,1))return false;requested=true;}
            if(GetTickCount64()>=until)return false;Sleep(5);
        }
    }return true;
}
struct OwnedTransferChild {
    platform::win32::installation::Handle job,process,thread;
    bool assigned{},stopped{};
    bool reap()noexcept{return stopped=stop_owned_process_job(job.get(),process.get(),assigned);}
    ~OwnedTransferChild(){if(!stopped)(void)reap();}
};
// Native host-owned disposable control work. Receipt validation must precede
// capture of helper-created outputs. Durable payloads/journals are not admitted.
// The host keeps this object through the authoritative completion/requery.
class OwnedTransferWork final {
    struct Proof {
        std::wstring name;BY_HANDLE_FILE_INFORMATION identity{};
        platform::win32::installation::Handle pin;
    };
    std::vector<Proof> proofs_;
    bool disposable_{};
    void warning(const char* code)noexcept{if(notice)try{notice(path.filename().u8string(),code);}catch(...){}}
public:
    std::filesystem::path path;std::wstring owner;
    platform::win32::installation::Handle directory;
    DWORD volume{};std::uint64_t identity{};
    bool children_stopped{};WorkProblemObserver notice;
    void capture(const wchar_t* leaf,HANDLE source){
        namespace ins=platform::win32::installation;
        const std::wstring name(leaf);ins::require(!name.empty()&&name.find_first_of(L"\\/:.")!=0&&name.find_first_of(L"\\/:")==name.npos,"work_capture_leaf_invalid");
        BY_HANDLE_FILE_INFORMATION info{};ins::require(GetFileInformationByHandle(source,&info)&&info.nNumberOfLinks==1&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"work_capture_regular_required");
        std::vector<wchar_t> final(32768);const auto count=GetFinalPathNameByHandleW(source,final.data(),static_cast<DWORD>(final.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        const auto expected=L"\\\\?\\"+(path/name).native();
        ins::require(count&&count<final.size()&&!_wcsicmp(std::wstring(final.data(),count).c_str(),expected.c_str()),"work_capture_current_path_mismatch");
        HANDLE duplicate{};ins::require(DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&duplicate,FILE_READ_ATTRIBUTES,FALSE,0),"work_capture_pin_failed");
        Proof proof{name,info,ins::Handle(duplicate)};proofs_.push_back(std::move(proof));
    }
    void confirm_completed()noexcept{if(children_stopped)disposable_=true;}
    ~OwnedTransferWork(){
        namespace ins=platform::win32::installation;
        if(path.empty())return;
        if(!directory){warning("job_setup_incomplete_work_preserved");return;}
        if(!disposable_||!children_stopped){warning("job_work_unconfirmed_preserved");return;}
        try{
            ins::require(ins::file_id(directory.get())==identity&&ins::volume_serial(directory.get())==volume,"work_directory_identity_changed");
            // The independent originals are gone before this last work owner.
            // Release our immutable read pins, then reopen and recheck EXACT
            // identity/size/time under a no-write/no-delete native capability.
            for(auto& proof:proofs_)proof.pin.reset();
            for(const auto& proof:proofs_){
                auto file=ins::open_object(path/proof.name,false,GENERIC_READ|DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);
                BY_HANDLE_FILE_INFORMATION actual{};const auto& prior=proof.identity;
                ins::require(GetFileInformationByHandle(file.get(),&actual)&&actual.nNumberOfLinks==1&&
                    actual.dwVolumeSerialNumber==prior.dwVolumeSerialNumber&&actual.nFileIndexHigh==prior.nFileIndexHigh&&actual.nFileIndexLow==prior.nFileIndexLow&&
                    actual.ftCreationTime.dwHighDateTime==prior.ftCreationTime.dwHighDateTime&&actual.ftCreationTime.dwLowDateTime==prior.ftCreationTime.dwLowDateTime&&
                    actual.ftLastWriteTime.dwHighDateTime==prior.ftLastWriteTime.dwHighDateTime&&actual.ftLastWriteTime.dwLowDateTime==prior.ftLastWriteTime.dwLowDateTime&&
                    actual.nFileSizeHigh==prior.nFileSizeHigh&&actual.nFileSizeLow==prior.nFileSizeLow,"work_file_changed_preserved");
                FILE_DISPOSITION_INFO remove{TRUE};ins::require(SetFileInformationByHandle(file.get(),FileDispositionInfo,&remove,sizeof(remove)),"work_owned_cleanup_failed");
                file.reset();const auto flags=GetFileAttributesW((path/proof.name).c_str());const auto error=GetLastError();
                ins::require(flags==INVALID_FILE_ATTRIBUTES&&error==ERROR_FILE_NOT_FOUND,"work_cleanup_absence_unconfirmed");
            }
            // Unknown objects retain the directory; never enumerate-and-delete.
            FILE_DISPOSITION_INFO remove{TRUE};ins::require(SetFileInformationByHandle(directory.get(),FileDispositionInfo,&remove,sizeof(remove)),"work_directory_not_empty_or_cleanup_failed");
        }catch(...){warning("job_owned_cleanup_incomplete_preserved");}
    }
};
}
