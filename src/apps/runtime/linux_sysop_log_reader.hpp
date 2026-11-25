#pragma once
#include "sfx/application/sysop_session.hpp"
#include "sfx/application/operational_log.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <array>
#include <cerrno>
#include <filesystem>
#include <deque>
#include <cstdio>

namespace sfx::apps::runtime {
inline application::SysopTextPage read_linux_sysop_log(const std::filesystem::path& root,
    application::SysopLog source,std::optional<std::uint64_t> requested,unsigned rows,
    unsigned columns,
    const std::function<bool()>& keep_going){
    using namespace application;
    namespace host=platform::linux_host;
    static const char* paths[]={"logs/heysysop.log","logs/callers.log","logs/bink.log","data/clamav/update-last.log","logs/sfxruntime.log"};
    static const char* titles[]={"heysysop.log","callers.log","bink.log","freshclam.log","BotGate Activity"};
    SysopTextPage out;const auto index=static_cast<unsigned>(source);if(index>=5){out.notice="Log source unavailable.";return out;}out.title=titles[index];
    const auto path=root/paths[index];host::FileDescriptor parent,file;struct stat identity{};
    const auto safe=[](const struct stat& info){return S_ISREG(info.st_mode)&&info.st_uid==geteuid()&&info.st_nlink==1&&(info.st_mode&022)==0&&info.st_size>=0;};
    try{
        if(!root.is_absolute()||root!=root.lexically_normal())throw std::runtime_error("log_root_invalid");
        parent=host::open_owned_directory(path.parent_path());
    }catch(...){out.notice="Log directory unavailable or redirected.";return out;}
    file=host::FileDescriptor(openat(parent.get(),path.filename().c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    if(!file){out.notice=errno==ENOENT?"Current log is missing.":errno==EACCES?"Access to the current log was denied.":"Current log is unavailable.";return out;}
    if(fstat(file.get(),&identity)!=0||!safe(identity)){out.notice="Current log identity is unavailable.";return out;}
    const auto captured=static_cast<std::uint64_t>(identity.st_size);
    crypto_hash_sha256_state hash{};std::array<unsigned char,32> first_hash{},second_hash{};
    std::uint64_t count{},unsupported{},long_lines{},read{};
    std::string line;
    bool oversized=false;
    unsigned per_page{};
    bool collecting=false;
    std::uint64_t selected_page{};
    const auto accept=[&]{
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::string shown;
        if(source==SysopLog::botgate){const auto decoded=oversized?std::optional<AlphaOperationalLogEntry>{}:decode_operational_record(line,count+unsupported+1);if(!decoded||!decoded->event){++unsupported;line.clear();oversized=false;return;}if(operational_filter_group(*decoded)!=3){line.clear();oversized=false;return;}
            const auto instant=static_cast<std::time_t>(decoded->utc_seconds);std::tm utc{};char stamp[40]{};
            if(static_cast<std::int64_t>(instant)==decoded->utc_seconds&&gmtime_r(&instant,&utc))std::strftime(stamp,sizeof stamp,"%Y-%m-%d %H:%M:%S UTC",&utc);
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
        char buffer[65536];if(crypto_hash_sha256_init(&hash)!=0){out.notice="Log verification unavailable.";return false;}
        while(read<captured){
            if(!keep_going()){out.notice="Log read cancelled or Sysop access changed.";return false;}
            const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(sizeof buffer,captured-read));
            ssize_t got;do{got=pread(file.get(),buffer,wanted,static_cast<off_t>(read));}while(got<0&&errno==EINTR);
            if(got<=0){out.notice="Log changed or became unreadable. Refresh to restart.";return false;}
            crypto_hash_sha256_update(&hash,reinterpret_cast<const unsigned char*>(buffer),static_cast<std::size_t>(got));read+=static_cast<std::uint64_t>(got);
            for(ssize_t i=0;i<got;++i){if(buffer[i]=='\n')accept();else if(line.size()<16384)line+=buffer[i];else oversized=true;}
        }
        return true;
    };
    // Count rendered rows and warning rows before choosing the page capacity.
    // A second bounded pass over this same handle collects only that page.
    if(!scan()){out.lines.clear();return out;}
    crypto_hash_sha256_final(&hash,first_hash.data());
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
    read=0;count=0;unsupported=0;long_lines=0;line.clear();oversized=false;collecting=true;
    if(!scan()){out.lines.clear();return out;}
    crypto_hash_sha256_final(&hash,second_hash.data());
    bool same=false;
    try{
        const auto current=host::open_owned_directory(path.parent_path());struct stat old_parent{},new_parent{},named{},final{};
        same=fstat(parent.get(),&old_parent)==0&&fstat(current.get(),&new_parent)==0&&old_parent.st_dev==new_parent.st_dev&&old_parent.st_ino==new_parent.st_ino&&
            fstatat(current.get(),path.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&safe(named)&&fstat(file.get(),&final)==0&&safe(final)&&final.st_size>=identity.st_size&&named.st_dev==identity.st_dev&&named.st_ino==identity.st_ino;
    }catch(...){same=false;}
    if(!same){out.lines.clear();out.notice="Current log was replaced or truncated. Refresh to restart on the new file.";return out;}
    if(first_hash!=second_hash){out.lines.clear();out.notice="Log content changed during read. Refresh to restart.";return out;}
    if(count!=total_rows||!keep_going()){out.lines.clear();out.notice="Log changed or access ended. Refresh to restart.";return out;}
    out.page=selected_page;
    out.previous=out.page>0;out.next=out.page<last;
    out.notice="Page "+std::to_string(out.page+1)+" of "+std::to_string(last+1);
    if(!details.empty())out.notice+='\n'+details;
    return out;
}
}
