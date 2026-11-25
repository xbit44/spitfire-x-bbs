#include "sfx/archive/qwk_archive.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <random>

namespace arc=sfx::archive;
namespace {
unsigned checks{};
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
struct Free {void operator()(struct archive* a)const{archive_write_free(a);}};
la_ssize_t append(struct archive*,void* context,const void* data,std::size_t length){static_cast<std::string*>(context)->append(static_cast<const char*>(data),length);return static_cast<la_ssize_t>(length);}
std::string zip(const std::vector<arc::QwkArchiveFile>& files,bool link=false){
    std::unique_ptr<struct archive,Free> writer(archive_write_new());std::string bytes;
    if(archive_write_set_format_zip(writer.get())!=ARCHIVE_OK || archive_write_set_bytes_per_block(writer.get(),0)!=ARCHIVE_OK || archive_write_open(writer.get(),&bytes,nullptr,append,nullptr)!=ARCHIVE_OK)throw std::runtime_error("test ZIP writer");
    for(const auto& file:files){
        auto* entry=archive_entry_new();archive_entry_set_pathname(entry,file.name.c_str());archive_entry_set_filetype(entry,link?AE_IFLNK:AE_IFREG);
        if(link)archive_entry_set_symlink(entry,"other");
        archive_entry_set_size(entry,link?0:static_cast<la_int64_t>(file.bytes.size()));
        if(archive_write_header(writer.get(),entry)!=ARCHIVE_OK){archive_entry_free(entry);throw std::runtime_error("test ZIP header");}
        if(!link && archive_write_data(writer.get(),file.bytes.data(),file.bytes.size())!=static_cast<la_ssize_t>(file.bytes.size())){archive_entry_free(entry);throw std::runtime_error("test ZIP data");}
        archive_entry_free(entry);
    }
    if(archive_write_close(writer.get())!=ARCHIVE_OK)throw std::runtime_error("test ZIP close");
    return bytes;
}
void le32(std::string& s,std::size_t p,std::uint32_t value){for(unsigned n=0;n<4;++n)s[p+n]=static_cast<char>(value>>(n*8));}
}
int main(){try{
    const auto ordinary=zip({{"SFX1.MSG","reply record bytes"},{"HEADERS.DAT","[80]\nSubject: subject\n"}});
    auto read=arc::read_qwk_reply_zip(ordinary,"SFX1");check(read.result.ok() && read.files.size()==2 && read.files[0].bytes=="reply record bytes","actual deflated ZIP read with CRC and count");
    for(const auto name:{"../SFX1.MSG","C:/SFX1.MSG","\\\\server\\SFX1.MSG","CON","SFX1.MSG:stream","SFX1.MSG/child","nested.zip","VOTING.DAT","TODOOR.EXT","file.exe"})check(!arc::read_qwk_reply_zip(zip({{name,"bad"}}),"SFX1").result.ok(),"unsafe/unsupported ZIP entry refused without extraction");
    check(!arc::read_qwk_reply_zip(zip({{"SFX1.MSG","a"},{"sfx1.msg","b"}}),"SFX1").result.ok(),"case collision refused");
    check(!arc::read_qwk_reply_zip(zip({{"SFX1.MSG","a"},{"HEADERS.DAT","b"},{"EXTRA.DAT","c"}}),"SFX1").result.ok(),"extra entries refused as whole packet");
    check(!arc::read_qwk_reply_zip(zip({{"SFX1.MSG",""}},true),"SFX1").result.ok(),"symlink entry refused");
    check(!arc::read_qwk_reply_zip(ordinary,"OTHER").result.ok(),"inner filename validated from configured board");
    check(!arc::read_qwk_reply_zip(ordinary.substr(0,ordinary.size()-1),"SFX1").result.ok(),"truncated directory refused");
    check(!arc::read_qwk_reply_zip(ordinary+"trailing payload","SFX1").result.ok(),"trailing payload refused");
    check(!arc::read_qwk_reply_zip("MZ"+ordinary,"SFX1").result.ok(),"embedded executable ZIP refused");
    auto wrong=ordinary;const auto central=wrong.find(std::string("PK\1\2",4));check(central!=std::string::npos,"central ZIP directory exists");
    le32(wrong,central+16,0x12345678);check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"wrong central CRC refused");
    wrong=ordinary;wrong[30]='X';check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"local/central filename mismatch refused");
    wrong=ordinary;wrong[30+4]='\0';wrong[central+46+4]='\0';check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"embedded NUL name cannot be accepted through a truncated C string");
    wrong=ordinary;wrong[central+42]=1;check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"invalid central local-record pointer refused");
    wrong=ordinary;wrong[8]=12;wrong[central+10]=12;check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"unqualified ZIP compression method refused");
    wrong=ordinary;wrong[6]=static_cast<char>(static_cast<unsigned char>(wrong[6])|1);wrong[central+8]=static_cast<char>(static_cast<unsigned char>(wrong[central+8])|1);
    check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"encrypted flags refused");
    const auto bomb=zip({{"SFX1.MSG",std::string(static_cast<std::size_t>(arc::qwk_expanded_limit)+1,'x')}});
    check(bomb.size()<arc::qwk_zip_limit && arc::read_qwk_reply_zip(bomb,"SFX1").result.error==arc::Error::archive_invalid,"oversized declared expanded entry refused");
    wrong=bomb;const auto bomb_central=wrong.find(std::string("PK\1\2",4));le32(wrong,bomb_central+24,1);
    check(!arc::read_qwk_reply_zip(wrong,"SFX1").result.ok(),"false expanded-size ZIP bomb refused by actual bytes or structural consistency");
    const auto exact=zip({{"SFX1.MSG",std::string(static_cast<std::size_t>(arc::qwk_expanded_limit),'x')}});read=arc::read_qwk_reply_zip(exact,"SFX1");check(read.result.ok() && read.files[0].bytes.size()==arc::qwk_expanded_limit,"exact actual expanded cap accepted");read={};
    check(arc::read_qwk_reply_zip(ordinary,"SFX1",[]{return true;}).result.error==arc::Error::cancelled,"read cancellation reported not success");
    check(arc::read_qwk_reply_zip(std::string(static_cast<std::size_t>(arc::qwk_zip_limit)+1,'x'),"SFX1").result.error==arc::Error::limit_exceeded,"actual compressed overcap rejected");
    const std::vector<arc::QwkArchiveFile> download{{"MESSAGES.DAT","message bytes"},{"CONTROL.DAT","control"},{"DOOR.ID","door"},{"001.NDX","index"}};
    const auto made=arc::write_qwk_zip(download);check(made.result.ok() && !made.zip.empty() && made.zip.substr(0,4)==std::string("PK\3\4",4),"actual complete ZIP generated");
    check(!arc::write_qwk_zip({{"MESSAGES.DAT","a"}}).result.ok(),"missing required control files refused");
    auto bad=download;bad.push_back({"messages.dat","duplicate"});check(!arc::write_qwk_zip(bad).result.ok(),"output duplicates refused");
    bad=download;bad.push_back({"../FILE","bad"});check(!arc::write_qwk_zip(bad).result.ok(),"output traversal refused");
    bad=download;bad[0].bytes=std::string(static_cast<std::size_t>(arc::qwk_expanded_limit),'x');check(arc::write_qwk_zip(bad).result.error==arc::Error::limit_exceeded,"metadata charged against output expanded limit");
    check(arc::write_qwk_zip(download,[]{return true;}).result.error==arc::Error::cancelled,"write cancellation isn't delivery success");
    std::minstd_rand rng(17);bad=download;bad[0].bytes.resize(static_cast<std::size_t>(arc::qwk_zip_limit)+65536);
    for(auto& c:bad[0].bytes)c=static_cast<char>(rng()&255);
    check(arc::write_qwk_zip(bad).result.error==arc::Error::limit_exceeded,"actual compressed output cap rejects complete random payload not truncates");
    std::cout<<"PASS qwk archive "<<checks<<" checks; engineering only, no filesystem/client acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL qwk archive after "<<checks<<": "<<e.what()<<'\n';return 1;}}
