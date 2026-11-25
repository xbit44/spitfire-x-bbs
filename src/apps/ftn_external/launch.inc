// Normal-user foreground launcher. No service installation, global setting,
// shell command construction or credential argument is involved.
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace {
int launch_stock(const std::filesystem::path& executable,const std::string& configuration,bool server_only){
    const auto binary=std::filesystem::absolute(executable).lexically_normal();
    if(!std::filesystem::is_regular_file(binary))throw std::runtime_error("ftn_external_mailer_not_found");
#ifdef _WIN32
    struct Handle {HANDLE value{};~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
    Handle job{CreateJobObjectW(nullptr,nullptr)};if(!job.value)throw std::runtime_error("ftn_external_mailer_job_failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw std::runtime_error("ftn_external_mailer_job_failed");
    auto command=L"\""+binary.native()+L"\" "+(server_only?std::wstring(L"-s "):std::wstring{})+L"\""+std::filesystem::u8path(configuration).native()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(binary.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED,nullptr,nullptr,&startup,&process))throw std::runtime_error("ftn_external_mailer_start_failed");
    Handle child{process.hProcess},thread{process.hThread};
    if(!AssignProcessToJobObject(job.value,child.value)||ResumeThread(thread.value)==static_cast<DWORD>(-1)){
        TerminateProcess(child.value,1);WaitForSingleObject(child.value,10000);throw std::runtime_error("ftn_external_mailer_ownership_failed");
    }
    if(WaitForSingleObject(child.value,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("ftn_external_mailer_wait_failed");
    DWORD code{};if(!GetExitCodeProcess(child.value,&code))throw std::runtime_error("ftn_external_mailer_exit_unavailable");return static_cast<int>(code);
#else
    umask(0077);
    if(server_only)execl(binary.c_str(),binary.c_str(),"-s",configuration.c_str(),static_cast<char*>(nullptr));
    else execl(binary.c_str(),binary.c_str(),configuration.c_str(),static_cast<char*>(nullptr));
    throw std::runtime_error("ftn_external_mailer_start_failed");
#endif
}
}
