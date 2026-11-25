#pragma once
#include "sfx/application/sysop_session.hpp"
#include "sfx/application/operational_log.hpp"
#include <windows.h>
#include <filesystem>
#include <deque>
#include <cstdio>

namespace sfx::apps::runtime {
inline application::SysopTextPage read_sysop_log(const std::filesystem::path& root,
    application::SysopLog source,std::optional<std::uint64_t> requested,unsigned rows,
    unsigned columns,
    const std::function<bool()>& keep_going){
    using namespace application;
    static const wchar_t* paths[]={L"Logs/HEYSYSOP.LOG",L"Logs/CALLERS.LOG",L"Logs/bink.log",L"Logs/ClamAV/freshclam.log",L"Logs/SFXRuntime.log"};
    static const char* titles[]={"HEYSYSOP.LOG","CALLERS.LOG","bink.log","freshclam.log","BotGate Activity"};
    SysopTextPage out;const auto index=static_cast<unsigned>(source);if(index>=5){out.notice="Log source unavailable.";return out;}out.title=titles[index];
    const auto path=root/paths[index];
    // Fixed current sources only. Reject redirected directory components and
    // final reparse points; do not change ACLs or deny sharing to log writers.
    for(auto p=path.parent_path();p!=root.parent_path()&&!p.empty();p=p.parent_path()){
        const auto attr=GetFileAttributesW(p.c_str());if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT)){out.notice="Log directory unavailable or redirected.";return out;}if(p==root)break;
    }
    struct Handle {HANDLE h{INVALID_HANDLE_VALUE};~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}} file;
    file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.h==INVALID_HANDLE_VALUE){const auto error=GetLastError();out.notice=error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?"Current log is missing.":error==ERROR_ACCESS_DENIED?"Access to the current log was denied.":"Current log is unavailable.";return out;}
    std::wstring final_path(32768,L'\0');const auto final_length=GetFinalPathNameByHandleW(file.h,final_path.data(),static_cast<DWORD>(final_path.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    auto expected=path;expected.make_preferred();const auto expected_name=L"\\\\?\\"+expected.wstring();
    if(!final_length||final_length>=final_path.size()){out.notice="Log path identity unavailable.";return out;}final_path.resize(final_length);
    if(_wcsicmp(final_path.c_str(),expected_name.c_str())!=0){out.notice="Log path changed or is redirected.";return out;}
    BY_HANDLE_FILE_INFORMATION identity{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(file.h,&identity)||identity.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)||!GetFileSizeEx(file.h,&size)||size.QuadPart<0){out.notice="Current log identity is unavailable.";return out;}
    std::uint64_t count{},unsupported{},long_lines{},read{};
    std::string line;
    bool oversized=false;
    unsigned per_page{};
    bool collecting=false;
    std::uint64_t selected_page{};
    const auto accept=[&]{
        if(!line.empty()&&line.back()=='\r')line.pop_back();std::string shown;
        if(source==SysopLog::botgate){const auto decoded=oversized?std::optional<AlphaOperationalLogEntry>{}:decode_operational_record(line,count+unsupported+1);if(!decoded||!decoded->event){++unsupported;line.clear();oversized=false;return;}if(operational_filter_group(*decoded)!=3){line.clear();oversized=false;return;}
            const auto ticks=(static_cast<std::uint64_t>(decoded->utc_seconds)+11644473600ULL)*10000000ULL;FILETIME ft{static_cast<DWORD>(ticks),static_cast<DWORD>(ticks>>32)};SYSTEMTIME st{};char stamp[40]{};
            if(FileTimeToSystemTime(&ft,&st))std::snprintf(stamp,sizeof stamp,"%04u-%02u-%02u %02u:%02u:%02u UTC",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
            shown=render_operational_line(*decoded,std::string(stamp));}
        else shown=line;
        if(oversized){++long_lines;shown+=" [line truncated]";}
        for(char& c:shown)if(static_cast<unsigned char>(c)<32||c==127)c=' ';
        for(auto& row:wrap_sysop_log_text(shown,columns)){
            if(collecting&&count/per_page==selected_page)out.lines.push_back(std::move(row));
            ++count;
        }
        line.clear();oversized=false;
    };
    const auto scan=[&]{
        char buffer[65536];
        while(read<static_cast<std::uint64_t>(size.QuadPart)){
            if(!keep_going()){out.notice="Log read cancelled or Sysop access changed.";return false;}
            DWORD got{};
            const auto wanted=static_cast<DWORD>(std::min<std::uint64_t>(sizeof buffer,static_cast<std::uint64_t>(size.QuadPart)-read));
            if(!ReadFile(file.h,buffer,wanted,&got,nullptr)||!got){out.notice="Log changed or became unreadable. Refresh to restart.";return false;}
            read+=got;
            for(DWORD i=0;i<got;++i){if(buffer[i]=='\n')accept();else if(line.size()<16384)line+=buffer[i];else oversized=true;}
        }
        return true;
    };
    // Count rendered rows and warning rows before choosing the page capacity.
    // A second bounded pass over this same handle collects only that page.
    if(!scan()){out.lines.clear();return out;}
    // A concurrent writer's incomplete final line is not a valid durable event.
    const bool partial=!line.empty()||oversized;
    const auto total_rows=count;
    std::string details;
    if(!count)details="Current log has no matching complete records.";
    if(unsupported)details+=(details.empty()?"":"\n")+std::to_string(unsupported)+" older, unsupported or malformed records could not be classified.";
    if(long_lines)details+=(details.empty()?"":"\n")+std::to_string(long_lines)+" overlong lines were bounded.";
    if(partial)details+=(details.empty()?"":"\n")+std::string("Incomplete final record omitted; Refresh after the writer completes it.");
    per_page=sysop_log_content_rows(out.title,details,count,columns,rows);
    if(!per_page){out.notice="Screen too small. Resize and Refresh.";return out;}
    const auto last=count?(count-1)/per_page:0;
    selected_page=requested.value_or(last);
    if(selected_page>last){out.notice="This page no longer exists. Refresh to open the latest page.";return out;}
    LARGE_INTEGER beginning{};
    if(!SetFilePointerEx(file.h,beginning,nullptr,FILE_BEGIN)){out.notice="Log position unavailable. Refresh to restart.";return out;}
    read=0;count=0;unsupported=0;long_lines=0;line.clear();oversized=false;collecting=true;
    if(!scan()){out.lines.clear();return out;}
    Handle current;current.h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);BY_HANDLE_FILE_INFORMATION now{};LARGE_INTEGER final_size{};
    if(current.h==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(current.h,&now)||!GetFileSizeEx(file.h,&final_size)||final_size.QuadPart<size.QuadPart||now.dwVolumeSerialNumber!=identity.dwVolumeSerialNumber||now.nFileIndexHigh!=identity.nFileIndexHigh||now.nFileIndexLow!=identity.nFileIndexLow){out.lines.clear();out.notice="Current log was replaced or truncated. Refresh to restart on the new file.";return out;}
    if(count!=total_rows||!keep_going()){out.lines.clear();out.notice="Log changed or access ended. Refresh to restart.";return out;}
    out.page=selected_page;
    out.previous=out.page>0;out.next=out.page<last;
    out.notice="Page "+std::to_string(out.page+1)+" of "+std::to_string(last+1);
    if(!details.empty())out.notice+='\n'+details;
    return out;
}
}
