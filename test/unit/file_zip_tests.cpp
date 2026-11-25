#include "sfx/archive/file_zip.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <iostream>
#include <stdexcept>
#include <memory>

namespace arc=sfx::archive;
namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool condition,const char* what){++checks;if(!condition)throw std::runtime_error(what);}
struct Free{void operator()(struct archive* a)const{archive_write_free(a);}};
la_ssize_t append(struct archive*,void* context,const void* bytes,std::size_t n){static_cast<std::string*>(context)->append(static_cast<const char*>(bytes),n);return static_cast<la_ssize_t>(n);}
std::string make(const std::vector<std::pair<std::string,std::string>>& files,bool link=false){
    std::unique_ptr<struct archive,Free> writer(archive_write_new());std::string out;
    if(archive_write_set_format_zip(writer.get())!=ARCHIVE_OK||archive_write_set_bytes_per_block(writer.get(),0)!=ARCHIVE_OK||archive_write_open(writer.get(),&out,nullptr,append,nullptr)!=ARCHIVE_OK)throw std::runtime_error("fixture ZIP initialization");
    for(const auto& file:files){auto* entry=archive_entry_new();archive_entry_set_pathname(entry,file.first.c_str());archive_entry_set_filetype(entry,link?AE_IFLNK:AE_IFREG);archive_entry_set_size(entry,link?0:static_cast<la_int64_t>(file.second.size()));if(link)archive_entry_set_symlink(entry,"not-extracted");
        if(archive_write_header(writer.get(),entry)!=ARCHIVE_OK){archive_entry_free(entry);throw std::runtime_error("fixture ZIP header");}
        if(!link&&archive_write_data(writer.get(),file.second.data(),file.second.size())!=static_cast<la_ssize_t>(file.second.size())){archive_entry_free(entry);throw std::runtime_error("fixture ZIP bytes");}archive_entry_free(entry);
    }
    if(archive_write_close(writer.get())!=ARCHIVE_OK)throw std::runtime_error("fixture ZIP finish");
    return out;
}
arc::FileZipSource source(const std::string& bytes){return {bytes.size(),[&bytes](std::uint64_t at,std::size_t n){if(n>65536||at>bytes.size()||n>bytes.size()-at)throw std::runtime_error("unbounded source read");return std::vector<std::uint8_t>(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+n));},{}};}
void le32(std::string& data,std::size_t at,std::uint32_t value){for(unsigned n=0;n<4;++n)data[at+n]=static_cast<char>(value>>(n*8));}
std::string raw_name_fixture(const std::string& name){
    // The writer normalizes backslashes. Replace same-length header bytes so
    // this negative test really contains the requested hostile wire name.
    auto bytes=make({{std::string(name.size(),'x'),"data"}});
    const auto central=bytes.find(std::string("PK\1\2",4));
    if(central==std::string::npos)throw std::runtime_error("fixture central header missing");
    bytes.replace(30,name.size(),name);bytes.replace(central+46,name.size(),name);
    return bytes;
}
}
int main(){try{
    const auto good=make({{"FILE_ID.DIZ","\r\n\x1b[31mUseful description\x1b[0m\r\nLong description.\r\n"},{"folder/readme.txt","Preview text"},{"program.exe",std::string("MZ\0binary",9)}});
    const auto original=good;auto view=arc::inspect_file_zip(source(good));check(view.result.ok()&&view.entries.size()==3&&!view.encrypted,"actual bounded ZIP listing");
    auto diz=arc::read_file_diz(source(good));check(diz.result.ok()&&diz.descriptions&&diz.descriptions->short_text=="Useful description"&&diz.descriptions->long_text.find('\x1b')==std::string::npos,"actual deflated DIZ plus safe description conversion");
    auto text=arc::read_file_zip_entry(source(good),1);check(text.result.ok()&&text.bytes=="Preview text","selected member decompression and CRC, no filesystem extraction");
    check(!arc::read_file_zip_entry(source(good),3).result.ok()&&!arc::read_file_zip_entry(source(good),1,4).result.ok(),"bad selection/expanded cap refused");
    for(const auto* name:{"../FILE_ID.DIZ","C:/FILE_ID.DIZ","/FILE_ID.DIZ","folder/../FILE_ID.DIZ","folder\\FILE_ID.DIZ","file:stream","folder//file","name. ","\x1b[31mname"}){
        const auto bad=raw_name_fixture(name);check(!arc::inspect_file_zip(source(bad)).result.ok(),"untrusted member names refused, never extracted");
    }
    const auto duplicate=make({{"FILE_ID.DIZ","one"},{"file_id.diz","two"}});check(!arc::read_file_diz(source(duplicate)).result.ok(),"ambiguous DIZ refused");
    const auto linked=make({{"FILE_ID.DIZ",""}},true);check(!arc::read_file_diz(source(linked)).result.ok(),"symlink DIZ refused");
    const auto none=make({{"ordinary.txt","no diz"}});check(arc::read_file_diz(source(none)).result.ok()&&!arc::read_file_diz(source(none)).descriptions,"missing DIZ normal fallback");
    const auto nested=make({{"folder/FILE_ID.DIZ","nested"}});check(!arc::read_file_diz(source(nested)).descriptions,"only unambiguous root DIZ selected");
    const auto lower=make({{"file_id.diz","lowercase"}});check(arc::read_file_diz(source(lower)).descriptions.has_value(),"case-insensitive root DIZ");
    auto changed=good;const auto central=changed.find(std::string("PK\1\2",4));check(central!=std::string::npos,"test central record");
    changed[6]=static_cast<char>(static_cast<unsigned char>(changed[6])|1);changed[central+8]=static_cast<char>(static_cast<unsigned char>(changed[central+8])|1);
    check(arc::inspect_file_zip(source(changed)).encrypted&&arc::read_file_diz(source(changed)).encrypted&&!arc::read_file_diz(source(changed)).result.ok(),"encrypted archive requires review, no fake DIZ success");
    changed=good;le32(changed,central+16,123456);check(!arc::read_file_diz(source(changed)).result.ok(),"wrong metadata CRC refused");
    changed=good;changed[30]='X';check(!arc::read_file_diz(source(changed)).result.ok(),"local/central name mismatch refused");
    changed=good;le32(changed,central+42,1);check(!arc::read_file_diz(source(changed)).result.ok(),"wrong local offset refused");
    changed=good;le32(changed,central+20,0xffffffff);check(!arc::read_file_diz(source(changed)).result.ok(),"unqualified ZIP64 marker refused");
    changed="MZ"+good;check(!arc::inspect_file_zip(source(changed)).result.ok(),"embedded executable not treated as plain ZIP");
    changed=good+"trailing bytes";check(!arc::inspect_file_zip(source(changed)).result.ok(),"unexpected trailing data refused");
    for(const auto block:{128U,1024U})for(const char fill:{'\x1a','\0'}){
        auto padded=good;const auto extra=(block-good.size()%block)%block;padded.append(extra,fill);
        const auto preserved=padded;const auto padded_diz=arc::read_file_diz(source(padded));
        check(padded_diz.result.ok()&&padded_diz.descriptions&&padded_diz.descriptions->short_text=="Useful description","XMODEM block fill permits exact ZIP/DIZ inspection");
        const auto padded_text=arc::read_file_zip_entry(source(padded),1);
        check(padded_text.result.ok()&&padded_text.bytes=="Preview text"&&padded==preserved,"padded ZIP preview verifies CRC without rewriting received bytes");
        if(extra){padded.back()='X';check(!arc::inspect_file_zip(source(padded)).result.ok(),"non-fill trailing payload refused");}
        padded=preserved+std::string(1024,fill);check(!arc::inspect_file_zip(source(padded)).result.ok(),"extra complete padding block refused");
        padded=preserved;padded[30]='X';check(!arc::read_file_diz(source(padded)).result.ok(),"padding does not bypass local and central identity validation");
        padded=preserved;le32(padded,central+16,123456);check(!arc::read_file_diz(source(padded)).result.ok(),"padding does not bypass CRC validation");
    }
    for(std::size_t n=0;n<good.size();++n){const auto truncated=good.substr(0,n);check(!arc::read_file_diz(source(truncated)).result.ok(),"all truncated ZIP boundaries refused");}
    const auto huge=make({{"FILE_ID.DIZ",std::string(app::kFileLongDescriptionBytes+1,'x')}});check(arc::read_file_diz(source(huge)).result.error==arc::Error::limit_exceeded,"expanded DIZ bound before decompression");
    auto cancelled=source(good);cancelled.cancelled=[]{return true;};check(arc::read_file_diz(cancelled).result.error==arc::Error::cancelled,"inspection cancellation distinct from absent DIZ");
    auto short_read=source(good);short_read.read=[](std::uint64_t,std::size_t){return std::vector<std::uint8_t>{};};check(!arc::inspect_file_zip(short_read).result.ok(),"short underlying reads fail closed");
    check(good==original,"no source rewrite");
    std::cout<<"PASS file ZIP/DIZ "<<checks<<" checks; bounded memory fixtures, not installed File Manager/client proof\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL file ZIP after "<<checks<<": "<<e.what()<<'\n';return 1;}}
