// Independent host-only verification. The test harness supplies owned fixture
// paths; production obtains its one target from GetSystemDirectoryW after the
// early x64 refusal. No environment/PATH override or installation operation.
#pragma once
#include <tlhelp32.h>
static fs::path system_driver(){std::array<wchar_t,512> path{};
    auto size=GetSystemDirectoryW(path.data(),static_cast<UINT>(path.size()));
    require(size&&size<path.size(),"native Windows system directory unavailable");
    return safe_path(fs::path(path.data())/L"SFXEXEC.DLL");}
static fs::path final_file_path(HANDLE file){std::array<wchar_t,1024> out{};
    auto size=GetFinalPathNameByHandleW(file,out.data(),static_cast<DWORD>(out.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    require(size&&size<out.size(),"pinned file pathname unavailable");std::wstring value(out.data(),size);
    require(value.rfind(L"\\\\?\\",0)==0,"local DOS-volume pathname required");return safe_path(fs::path(value.substr(4)));}
static void same_pinned_file(const fs::path& actual_path,HANDLE expected,const char* hash){
    Handle actual(CreateFileW(safe_path(actual_path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION a{},b{};
    require(actual.h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(actual.h,&a)&&GetFileInformationByHandle(expected,&b),"loaded module file identity unavailable");
    require(!(a.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&a.nNumberOfLinks==1&&b.nNumberOfLinks==1,"redirected/linked module refused");
    require(_wcsicmp(final_file_path(actual.h).c_str(),final_file_path(expected).c_str())==0&&a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow,"driver loaded from unexpected path/file");
    require(digest(final_file_path(actual.h))==hash,"actual loaded driver hash mismatch");}
static void actual_vdm_module(DWORD pid,HANDLE expected){
    Handle snapshot;for(unsigned tries=0;tries<10;++tries){snapshot.h=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid);
        if(snapshot.h!=INVALID_HANDLE_VALUE||GetLastError()!=ERROR_BAD_LENGTH)break;Sleep(10);}
    require(snapshot.h!=INVALID_HANDLE_VALUE,"actual VDM module inventory unavailable");MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);unsigned matches=0;
    require(Module32FirstW(snapshot.h,&entry),"actual VDM module list empty");
    do{if(_wcsicmp(entry.szModule,L"SFXEXEC.DLL")==0){++matches;same_pinned_file(fs::path(entry.szExePath),expected,SFX_DLL_SHA256);}}while(Module32NextW(snapshot.h,&entry));
    require(matches==1,"expected SFX driver not uniquely loaded in owned VDM");}
