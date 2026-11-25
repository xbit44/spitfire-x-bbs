#pragma once
#include <windows.h>
#include <filesystem>
namespace sfx::apps::runtime {
// SFXEXEC is the installed Door configuration, not a transfer-helper INI.
// Only this exact basename in Bin is exempt; Tools and private jobs stay empty.
inline bool transfer_ini_directory_clear(const std::filesystem::path& directory,bool door_config=false){
    WIN32_FIND_DATAW data{};const auto find=FindFirstFileW((directory/L"*.ini").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    bool safe=true;
    do {if(!door_config||_wcsicmp(data.cFileName,L"sfxexec.ini")!=0||(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))safe=false;} while(FindNextFileW(find,&data));
    const auto error=GetLastError();FindClose(find);return safe&&error==ERROR_NO_MORE_FILES;
}
}
