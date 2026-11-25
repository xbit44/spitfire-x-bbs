#include "sfx/platform/linux/durable_journal_files.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

namespace host=sfx::platform::linux_host;
namespace app=sfx::application;
namespace fs=std::filesystem;
using Kind=app::DurableJournalKind;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void put(const fs::path& path,std::string_view text) {
    auto dir=host::open_owned_directory(path.parent_path());
    host::publish_private_file(dir.get(),path.filename().string(),{text.begin(),text.end()},false);
}
std::size_t archives(const fs::path& logs,std::string_view prefix) {
    std::size_t count{};
    for(const auto& file:fs::directory_iterator(logs))
        if(file.path().filename().string().rfind(prefix,0)==0)++count;
    return count;
}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and fresh fixture required");
    const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh absolute fixture");
    check(fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"private root");
    for(const auto* leaf:{"logs","config"})check(fs::create_directory(root/leaf)&&chmod((root/leaf).c_str(),0700)==0,"private subtree");
    host::LinuxDurableJournalFiles files(root);
    const auto initial=files.inspect(Kind::callers);check(initial.has_value(),"create caller journal");
    const std::string header="; SFX CALLERS.LOG v1\r\n";
    check(initial->bytes==header.size()&&files.read(Kind::callers,0,header.size())==header,"canonical CRLF header");
    check(!files.read(Kind::callers,initial->bytes+1,0),"read beyond EOF refused");
    check(!files.append(Kind::callers,{"other",initial->bytes},"row\r\n---\r\n"),"stale identity refused");
    const std::string row="caller fixture\r\n---\r\n";
    check(files.append(Kind::callers,*initial,row),"durable append");
    check(!files.append(Kind::callers,*initial,row),"stale byte cursor refused");
    check(files.read(Kind::callers,header.size(),row.size())==row,"exact appended bytes");
    check(!files.append(Kind::callers,*files.inspect(Kind::callers),std::string(app::kCallerJournalRecordBytes+1,'x')),"oversized append refused");
    check(files.archive_and_reopen(Kind::callers),"archive preserves current journal");
    const auto fresh=files.inspect(Kind::callers);
    check(fresh&&fresh->bytes==header.size()&&fresh->identity!=initial->identity,"rotation creates fresh identity and header");
    check(archives(root/"logs","callers-")==1,"one recognized archive");
    app::LoggingConfiguration limits;limits.callers.archives=2;
    check(files.configure(limits),"bounded retention accepted");
    put(root/"logs"/"callers-owner-notes.LOG","owner notes preserved");
    for(unsigned i=0;i<4;++i) {
        check(files.append(Kind::callers,*files.inspect(Kind::callers),row)&&files.archive_and_reopen(Kind::callers),"append rotate prune");
    }
    check(archives(root/"logs","callers-")==3,"two recognized archives plus unrelated notes");
    check(fs::file_size(root/"logs"/"callers-owner-notes.LOG")==21,"unrelated file preserved");
    const auto before=files.inspect(Kind::callers);
    fs::rename(root/"logs"/"callers.log",root/"logs"/"held-original");
    put(root/"logs"/"callers.log",header);
    check(!files.inspect(Kind::callers)&&!files.append(Kind::callers,*before,row),"cached descriptor namespace replacement refused");
    check(fs::file_size(root/"logs"/"held-original")==header.size(),"original preserved after replacement refusal");
    fs::rename(root/"logs"/"callers.log",root/"logs"/"replacement-evidence");
    fs::rename(root/"logs"/"held-original",root/"logs"/"callers.log");
    check(files.inspect(Kind::callers).has_value(),"restored exact inode available");
    fs::rename(root/"logs"/"callers.log",root/"logs"/"fifo-held-original");
    check(mkfifo((root/"logs"/"callers.log").c_str(),0600)==0,"create journal FIFO replacement without writer");
    check(!files.inspect(Kind::callers)&&!files.archive_and_reopen(Kind::callers),"cached journal FIFO refuses read and rotation without waiting");
    check(fs::is_fifo(root/"logs"/"callers.log")&&fs::file_size(root/"logs"/"fifo-held-original")==header.size(),"FIFO refusal preserves both replacement and held original");
    check(unlink((root/"logs"/"callers.log").c_str())==0,"remove only added journal FIFO");
    fs::rename(root/"logs"/"fifo-held-original",root/"logs"/"callers.log");
    check(files.inspect(Kind::callers).has_value(),"original journal remains usable");
    const auto attention=files.inspect(Kind::attention);check(attention.has_value(),"create attention journal");
    check(files.append(Kind::attention,*attention,"partial"),"retain incomplete write for recovery");
    check(!files.archive_and_reopen(Kind::attention),"partial record cannot be archived or discarded");
    check(files.read(Kind::attention,attention->bytes,7)=="partial","partial bytes retained");
    check(files.append(Kind::attention,*files.inspect(Kind::attention),"\r\n")&&files.archive_and_reopen(Kind::attention),"complete pending record before rotation");
    check(chmod((root/"logs"/"heysysop.log").c_str(),0640)==0&&!files.inspect(Kind::attention),"shared log refused without permission repair");
    check(chmod((root/"logs"/"heysysop.log").c_str(),0600)==0,"restore owned test mode");
    check(link((root/"logs"/"heysysop.log").c_str(),(root/"logs"/"extra-link").c_str())==0&&!files.inspect(Kind::attention),"hardlinked log refused");
    check(unlink((root/"logs"/"extra-link").c_str())==0,"remove only owned test hardlink");
    const auto runtime=files.inspect(Kind::runtime);check(runtime&&runtime->bytes==0,"empty operational journal accepted");
    check(files.append(Kind::runtime,*runtime,"bad record\r\n"),"write malformed recovery fixture");
    host::LinuxDurableJournalFiles restarted(root);
    check(!restarted.inspect(Kind::runtime),"malformed operational history blocks reopen");
    check(fs::file_size(root/"logs"/"sfxruntime.log")==12,"bad history preserved");
    check(host::load_new_caller_names(root).valid(),"missing optional JOKER list allowed");
    put(root/"config"/"joker.dat","ReservedName\r\n");
    const auto names=host::load_new_caller_names(root);
    check(names.valid()&&names.evaluate("ReservedName")==app::NewCallerNameDecision::unavailable,"shared name parser applied");
    check(chmod((root/"config"/"joker.dat").c_str(),0644)==0&&!host::load_new_caller_names(root).valid(),"shared name list fails closed");
    check(!files.inspect(static_cast<Kind>(99)),"invalid journal kind refused");
    std::cout<<"PASS "<<checks<<" actual Linux durable journal/recovery/retention checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
