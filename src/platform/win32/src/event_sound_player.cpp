#include "sfx/platform/win32/event_sound_player.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>

namespace sfx::platform::win32 {
namespace {
constexpr std::size_t maximum_bytes = 2U * 1024U * 1024U;
struct Close { void operator()(void* h) const noexcept { if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h); } };
using Handle = std::unique_ptr<void,Close>;
std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | p[1]<<8U); }
std::uint32_t u32(const unsigned char* p) { return p[0] | (std::uint32_t(p[1])<<8U) | (std::uint32_t(p[2])<<16U) | (std::uint32_t(p[3])<<24U); }
bool pcm_wave(const std::vector<unsigned char>& b,std::uint64_t& duration_ms) {
    if(b.size()<44 || std::memcmp(b.data(),"RIFF",4) || std::memcmp(b.data()+8,"WAVE",4) ||
        u32(b.data()+4)!=b.size()-8)return false;
    bool format{},data{};std::uint32_t rate{},length{};std::uint16_t block{};
    for(std::size_t at=12;at<b.size();) {
        if(b.size()-at<8)return false;
        const auto n=u32(b.data()+at+4);const auto start=at+8;
        if(n>b.size()-start)return false;
        if(!std::memcmp(b.data()+at,"fmt ",4)) {
            // PCM may use WAVEFORMAT or WAVEFORMATEX with no extension.
            if(format || (n!=16 && (n!=18 || u16(b.data()+start+16)!=0)))return false;format=true;
            const auto* p=b.data()+start;const auto channels=u16(p+2),bits=u16(p+14);
            const auto hz=u32(p+4);rate=u32(p+8);block=u16(p+12);
            if(u16(p)!=1 || (channels!=1 && channels!=2) || (bits!=8 && bits!=16) ||
                hz<8000 || hz>48000 || block!=channels*(bits/8) || rate!=hz*block)return false;
        } else if(!std::memcmp(b.data()+at,"data",4)) {
            if(data)return false;data=true;length=n;
        }
        // RIFF metadata (e.g. fact, LIST and CDif) is not audio. Skip it using
        // the same checked length/padding as every chunk. The fmt/data checks
        // still require bounded, uncompressed PCM; no new codec is enabled.
        const auto next=start+n+(n&1U);if(next>b.size())return false;at=next;
    }
    if(!format || !data || !length || !block || length%block || length>std::uint64_t(rate)*30U)return false;
    duration_ms=(std::uint64_t(length)*1000U+rate-1U)/rate;return true;
}
}
SoundFileResult read_event_sound(const std::wstring& path) {
    const auto fail=[](const wchar_t* why){return SoundFileResult{{},why};};
    if(path.size()<7 || path.size()>259 || !iswalpha(path[0]) || path[1]!=L':' || path[2]!=L'\\' ||
        path.find_first_of(L"/\r\n\t*?\"<>|")!=std::wstring::npos || path.find(L':',2)!=std::wstring::npos ||
        path.find(L'\0')!=std::wstring::npos)return fail(L"Choose an absolute local WAV path (drive:\\folder\\file.wav).");
    const auto extension=std::filesystem::path(path).extension().wstring();
    if(_wcsicmp(extension.c_str(),L".wav")!=0)return fail(L"Choose a .wav file.");
    if(GetDriveTypeW(path.substr(0,3).c_str())!=DRIVE_FIXED)return fail(L"Choose a WAV on a local fixed drive.");
    // Retain each non-reparse ancestor against rename/delete while reading.
    std::vector<Handle> directories;
    for(std::size_t end=2;end<path.size();) {
        const auto part=path.substr(0,end==2?3:end);
        Handle h(CreateFileW(part.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if(!h || h.get()==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h.get(),&info) ||
            !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
            return fail(L"The WAV folder is unavailable or uses a link/junction.");
        directories.push_back(std::move(h));
        const auto next=path.find(L'\\',end+1);if(next==std::wstring::npos)break;
        const auto component=path.substr(end+1,next-end-1);
        if(component.empty() || component==L"." || component==L".." || component.back()==L'.' || component.back()==L' ')
            return fail(L"Choose a normal local path without dot or empty components.");
        end=next;
    }
    const auto leaf=std::filesystem::path(path).filename().wstring();
    if(leaf.empty() || leaf.back()==L'.' || leaf.back()==L' ')return fail(L"Choose a normal WAV filename.");
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!file || file.get()==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(),&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || GetFileType(file.get())!=FILE_TYPE_DISK ||
        !GetFileSizeEx(file.get(),&size) || size.QuadPart<44 || size.QuadPart>maximum_bytes)
        return fail(L"The WAV is missing, locked, unsafe, or larger than 2 MiB.");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read{};
    std::uint64_t duration_ms{};
    if(!ReadFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) || read!=bytes.size() || !pcm_wave(bytes,duration_ms))
        return fail(L"Use PCM WAV: 8/16-bit, mono/stereo, 8-48 kHz, at most 30 seconds and 2 MiB.");
    return {std::move(bytes),L"WAV validated.",duration_ms};
}
EventSoundPlayer::~EventSoundPlayer(){stop();}
void EventSoundPlayer::stop() noexcept {
    if(!bytes_.empty()) { PlaySoundW(nullptr,nullptr,0);bytes_.clear(); }
    end_ms_=0;
}
bool EventSoundPlayer::playing() const noexcept{return !bytes_.empty()&&GetTickCount64()<end_ms_;}
bool EventSoundPlayer::preview(const std::wstring& path,std::wstring& detail) {
    stop();auto input=read_event_sound(path);detail=input.detail;if(!input)return false;
    bytes_=std::move(input.bytes);
    if(!PlaySoundW(reinterpret_cast<LPCWSTR>(bytes_.data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT)) {
        bytes_.clear();detail=L"Windows could not play this WAV. Check the local audio device.";return false;
    }
    end_ms_=GetTickCount64()+input.duration_ms+100;
    detail=L"Preview started. Stop ends it. No BBS event was created.";return true;
}
}
