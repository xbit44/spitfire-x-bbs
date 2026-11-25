#pragma once
#include "sfx/application/newsletter_editor.hpp"
#include <filesystem>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <sodium.h>
#if defined(_WIN32)
#include "sfx/platform/win32/installation_security.hpp"
#else
#include "sfx/platform/linux/private_files.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace sfx::apps::runtime {
// Fixed newsletter names only. Native publication retains the displaced file
// until its identity/bytes and the ANSI guard have been checked again.
class NewsletterFileStore final {
    using Snapshot=application::NewsletterSnapshot;
    std::filesystem::path directory_;
    static std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=char(c+32);return s;}
    static void require(bool value,const char* reason="newsletter_file_guard"){if(!value)throw std::runtime_error(reason);}
#if defined(_WIN32)
    using Handle=platform::win32::installation::Handle;
    static Handle open(const std::filesystem::path& path,bool directory){return platform::win32::installation::open_object(path,directory,GENERIC_READ,directory?(FILE_SHARE_READ|FILE_SHARE_WRITE):(FILE_SHARE_READ|FILE_SHARE_DELETE));}
    static std::string identity(HANDLE file){BY_HANDLE_FILE_INFORMATION s{};require(GetFileInformationByHandle(file,&s)&&s.nNumberOfLinks==1&&!(s.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)));return std::to_string(s.dwVolumeSerialNumber)+":"+std::to_string(s.nFileIndexHigh)+":"+std::to_string(s.nFileIndexLow);}
    static Snapshot read(const std::filesystem::path& path){
        auto file=open(path,false);Snapshot result;result.identity=identity(file.get());LARGE_INTEGER size{};require(GetFileSizeEx(file.get(),&size)&&size.QuadPart>=0&&size.QuadPart<=application::NewsletterDocument::maximum_bytes);
        result.bytes.resize(static_cast<std::size_t>(size.QuadPart));DWORD got{};require(result.bytes.empty()||(ReadFile(file.get(),result.bytes.data(),static_cast<DWORD>(result.bytes.size()),&got,nullptr)&&got==result.bytes.size()));
        result.exists=result.available=true;return result;
    }
    static void create(const std::filesystem::path& path,std::string_view bytes){
        const auto raw=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(raw!=INVALID_HANDLE_VALUE);Handle file(raw);DWORD written{};
        require((bytes.empty()||(WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()))&&FlushFileBuffers(file.get()));
    }
#else
    using Handle=platform::linux_host::FileDescriptor;
    static Handle open(const std::filesystem::path& path,bool directory){if(directory)return platform::linux_host::open_owned_directory(path,false);return Handle(::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));}
    static std::string identity(int file){struct stat s{};require(file>=0&&fstat(file,&s)==0&&S_ISREG(s.st_mode)&&s.st_uid==geteuid()&&s.st_nlink==1&&!(s.st_mode&022));return std::to_string(s.st_dev)+":"+std::to_string(s.st_ino);}
    static Snapshot read(const std::filesystem::path& path){
        auto file=open(path,false);Snapshot result;result.identity=identity(file.get());struct stat before{},after{};require(fstat(file.get(),&before)==0&&before.st_size>=0&&static_cast<std::uint64_t>(before.st_size)<=application::NewsletterDocument::maximum_bytes);
        result.bytes.resize(static_cast<std::size_t>(before.st_size));std::size_t at{};while(at<result.bytes.size()){const auto n=::read(file.get(),result.bytes.data()+at,result.bytes.size()-at);if(n<0&&errno==EINTR)continue;require(n>0);at+=static_cast<std::size_t>(n);}
        require(fstat(file.get(),&after)==0&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec);
        result.exists=result.available=true;return result;
    }
    static void create(const std::filesystem::path& path,std::string_view bytes){
        Handle file(::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));require(bool(file));std::size_t at{};
        while(at<bytes.size()){const auto n=::write(file.get(),bytes.data()+at,bytes.size()-at);if(n<0&&errno==EINTR)continue;require(n>0);at+=static_cast<std::size_t>(n);}require(fsync(file.get())==0);
    }
#endif
    std::string leaf()const{
        std::string selected;std::size_t count{};
        for(const auto& item:std::filesystem::directory_iterator(directory_)){
            require(++count<=4096);const auto name=item.path().filename().string(),folded=lower(name);
            if(folded=="sfnwsltr.ans")throw std::runtime_error("ansi");
            if(folded=="sfnwsltr.asc"){require(selected.empty());selected=name;}
        }
        return selected;
    }
    static bool same(const Snapshot& a,const Snapshot& b){return a.available&&b.available&&a.exists==b.exists&&a.identity==b.identity&&a.bytes==b.bytes;}
public:
#if defined(_WIN32)
    explicit NewsletterFileStore(std::filesystem::path root):directory_(std::move(root)/"Display"){}
#else
    explicit NewsletterFileStore(std::filesystem::path root):directory_(std::move(root)/"display"){}
#endif
    Snapshot load()const{
        try{auto pin=open(directory_,true);require(bool(pin));const auto name=leaf();if(name.empty())return {true,false,{},{},"No newsletter exists. Save creates SFNWSLTR.ASC."};
            auto result=read(directory_/name);result.identity=name+"\n"+result.identity;return result;
        }catch(const std::exception& e){return {false,false,{},{},std::string(e.what())=="ansi"?"An ANSI newsletter exists. This editor protects that artwork and leaves both files unchanged.":"Newsletter unavailable: check its ownership, file type, size and duplicate names. Nothing changed."};}
    }
    application::NewsletterSaveResult save(const Snapshot& expected,std::string_view bytes,const std::function<bool()>& authorized)const{
        std::filesystem::path staged,displaced;bool published=false;
        try{
            require(authorized&&authorized(),"newsletter_authority");application::NewsletterDocument validation;require(validation.load(std::string(bytes)),"newsletter_document_invalid");
            auto pin=open(directory_,true);require(bool(pin));
#if defined(_WIN32)
            const auto raw_lock=CreateFileW((directory_/L".sfnwsltr.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);require(raw_lock!=INVALID_HANDLE_VALUE);Handle lock(raw_lock);(void)identity(lock.get());
#else
            Handle lock(::open((directory_/".sfnwsltr.lock").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600));require(bool(lock));(void)identity(lock.get());require(flock(lock.get(),LOCK_EX|LOCK_NB)==0);
#endif
            require(same(expected,load()),"newsletter_snapshot_changed");
            if(expected.exists&&bytes==expected.bytes)return {true,"Newsletter unchanged; no file was rewritten."};
            const auto name=leaf();
#if defined(_WIN32)
            const auto target=directory_/(name.empty()?"SFNWSLTR.ASC":name);
#else
            const auto target=directory_/(name.empty()?"sfnwsltr.asc":name);
#endif
            std::array<unsigned char,16> random{};std::array<char,33> hex{};require(sodium_init()>=0);randombytes_buf(random.data(),random.size());sodium_bin2hex(hex.data(),hex.size(),random.data(),random.size());
            staged=directory_/(std::string(".sfnwsltr-stage-")+hex.data());displaced=directory_/(std::string(".sfnwsltr-previous-")+hex.data());
            create(staged,bytes);const auto prepared=read(staged);require(prepared.bytes==bytes,"newsletter_stage_mismatch");
            require(authorized()&&same(expected,load()),"newsletter_prepublication_changed");
            if(expected.exists){
#if defined(_WIN32)
                if(!ReplaceFileW(target.c_str(),staged.c_str(),displaced.c_str(),0,nullptr,nullptr))throw std::runtime_error("newsletter_replace_failed_"+std::to_string(GetLastError()));
#else
                // Atomic exchange makes concurrent replacement observable: its
                // exact inode remains at the stage name, never discarded blind.
                require(syscall(SYS_renameat2,AT_FDCWD,staged.c_str(),AT_FDCWD,target.c_str(),2)==0);
                displaced=staged;
#endif
                published=true;auto old=read(displaced);old.identity=name+"\n"+old.identity;
                const auto current=read(target);
                bool guarded=same(expected,old)&&current.identity==prepared.identity&&current.bytes==bytes&&authorized();
                try{guarded=guarded&&leaf()==name;}catch(...){guarded=false;}
                if(!guarded){
                    // Restore only while the destination is still our image.
                    if(current.identity==prepared.identity&&current.bytes==bytes){
#if defined(_WIN32)
                        require(ReplaceFileW(target.c_str(),displaced.c_str(),staged.c_str(),0,nullptr,nullptr)!=FALSE);
#else
                        require(syscall(SYS_renameat2,AT_FDCWD,displaced.c_str(),AT_FDCWD,target.c_str(),2)==0);
#endif
                    }
                    return {false,"Newsletter changed during Save. Original/concurrent content and the draft are retained; check the Display folder before trying again."};
                }
                require(std::filesystem::remove(displaced));
            }else{
#if defined(_WIN32)
                require(MoveFileExW(staged.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE);
#else
                require(syscall(SYS_renameat2,AT_FDCWD,staged.c_str(),AT_FDCWD,target.c_str(),1)==0);
#endif
                published=true;
                bool guarded=authorized();try{guarded=guarded&&leaf()==target.filename().string();}catch(...){guarded=false;}
                if(!guarded){const auto current=read(target);if(current.identity==prepared.identity&&current.bytes==bytes)std::filesystem::rename(target,staged);return {false,"Save stopped after an access or ANSI change. Draft retained in Display; reopen after review."};}
            }
#if !defined(_WIN32)
            require(fsync(pin.get())==0);
#endif
            return {true,"SFX Newsletter saved."};
        }catch(const std::exception& error){
#if defined(SFX_NEWSLETTER_FILE_TEST_DIAGNOSTICS)
            std::fprintf(stderr,"Newsletter save guard: %s\n",error.what());
#else
            (void)error;
#endif
            if(!published&&!staged.empty()){std::error_code ignored;std::filesystem::remove(staged,ignored);}
            return {false,published?"Save outcome needs review. Retained newsletter files are in Display; do not repeat Save until checked.":"Newsletter not saved: access, ANSI artwork or the original file changed, or safe publication is unavailable. Your draft is retained."};
        }
    }
};
}
