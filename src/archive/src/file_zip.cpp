#include "sfx/archive/file_zip.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <memory>
#include <set>

namespace sfx::archive {
namespace {
struct Failure {Error code;const char* detail;};
void need(bool value,Error code=Error::archive_invalid,const char* detail="Invalid or unsupported ZIP metadata."){if(!value)throw Failure{code,detail};}
std::uint16_t u16(std::string_view s,std::size_t n){need(n<=s.size()&&s.size()-n>=2);return static_cast<std::uint16_t>(static_cast<unsigned char>(s[n])|(static_cast<unsigned char>(s[n+1])<<8));}
std::uint32_t u32(std::string_view s,std::size_t n){return static_cast<std::uint32_t>(u16(s,n))|(static_cast<std::uint32_t>(u16(s,n+2))<<16);}
void put16(std::string& s,std::size_t n,std::uint16_t value){for(unsigned k=0;k<2;++k)s[n+k]=static_cast<char>(value>>(k*8));}
void put32(std::string& s,std::size_t n,std::uint32_t value){for(unsigned k=0;k<4;++k)s[n+k]=static_cast<char>(value>>(k*8));}
std::string folded(std::string s){for(auto& c:s)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return s;}
bool member_name(std::string_view name){
    if(name.empty()||name.size()>512||name.front()=='/'||name.find_first_of("\\:")!=std::string_view::npos)return false;
    for(unsigned char c:name)if(c<32||c==127)return false;
    if(name.back()=='/')name.remove_suffix(1);
    unsigned depth{};while(!name.empty()){
        const auto end=name.find('/');const auto part=name.substr(0,end);
        if(part.empty()||part=="."||part==".."||part.back()==' '||part.back()=='.'||++depth>16)return false;
        if(end==std::string_view::npos)return true;
        name.remove_prefix(end+1);
    }return false;
}
struct Reader {
    const FileZipSource& source;std::uint64_t spent{};
    void cancel(){need(!source.cancelled||!source.cancelled(),Error::cancelled,"ZIP inspection cancelled.");}
    std::string read(std::uint64_t offset,std::size_t count){
        cancel();need(source.read&&offset<=source.bytes&&count<=source.bytes-offset,Error::source_io,"ZIP source range unavailable.");
        need(count<=8U*1024U*1024U-spent,Error::limit_exceeded,"ZIP inspection input limit reached.");spent+=count;
        std::string result;result.reserve(count);
        while(result.size()<count){cancel();const auto wanted=std::min<std::size_t>(65536,count-result.size());auto bytes=source.read(offset+result.size(),wanted);need(bytes.size()==wanted,Error::source_io,"ZIP source changed or short read.");result.append(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
        return result;
    }
};
struct Entry {FileZipEntry visible;std::string central;std::uint32_t offset{},compressed{},crc{};std::uint16_t flags{},method{};};
struct Directory {std::vector<Entry> entries;std::uint64_t offset{};bool encrypted{};};
Directory inspect(Reader& reader){
    need(reader.source.bytes>=22&&reader.source.bytes<UINT32_MAX,Error::archive_invalid,"Only ordinary single-disk ZIP inspection is supported.");
    const auto initial=reader.read(0,4);need(initial==std::string("PK\3\4",4)||initial==std::string("PK\5\6",4));
    const auto tail_size=static_cast<std::size_t>(std::min<std::uint64_t>(65557+1023,reader.source.bytes));const auto tail=reader.read(reader.source.bytes-tail_size,tail_size);
    std::optional<std::size_t> end;
    // XMODEM has no file-length field. Keep every received byte on disk and
    // in hashing/scanning/accounting. For general File Area ZIP inspection only,
    // recognize uniform Ctrl-Z or zero fill within the final 128/1024-byte
    // block. Never discard arbitrary trailing data or a whole additional block.
    std::size_t fill=tail.size();
    if(!tail.empty()&&(tail.back()=='\x1a'||tail.back()=='\0')){
        while(fill&&tail[fill-1]==tail.back()&&tail.size()-fill<1023)--fill;
    }
    for(std::size_t n=tail.size()-22;;--n){
        if(tail.substr(n,4)==std::string("PK\5\6",4)){
            const auto comment=static_cast<std::size_t>(u16(tail,n+20));
            if(comment<=tail.size()-n-22){
                const auto finish=n+22+comment,padding=tail.size()-finish;
                const bool block_fill=padding&&finish>=fill&&
                    ((reader.source.bytes%128==0&&padding<128)||(reader.source.bytes%1024==0&&padding<1024));
                if(!padding||block_fill){need(!end);end=n;}
            }
        }
        if(!n)break;
    }
    need(end.has_value());const auto e=*end;const auto count=u16(tail,e+10);const auto central_size=u32(tail,e+12),central_offset=u32(tail,e+16);
    need(u16(tail,e+4)==0&&u16(tail,e+6)==0&&count==u16(tail,e+8)&&count!=65535&&count<=10000);
    const auto end_absolute=reader.source.bytes-tail_size+e;
    need(central_offset<=end_absolute&&central_size==end_absolute-central_offset&&central_size<=4U*1024U*1024U,Error::limit_exceeded,"ZIP directory is unsupported or too large.");
    const auto central=reader.read(central_offset,central_size);Directory result;result.offset=central_offset;std::set<std::string> names;std::size_t at{};
    for(unsigned n=0;n<count;++n){
        reader.cancel();need(at<=central.size()&&central.size()-at>=46&&central.substr(at,4)==std::string("PK\1\2",4));
        const auto name_size=u16(central,at+28),extra=u16(central,at+30),comment=u16(central,at+32);const auto size=46U+static_cast<std::size_t>(name_size)+extra+comment;
        need(size<=central.size()-at&&u16(central,at+34)==0);
        Entry entry;entry.visible.name=central.substr(at+46,name_size);need(member_name(entry.visible.name)&&names.insert(folded(entry.visible.name)).second,Error::unsafe_path,"Unsafe or ambiguous ZIP member name.");
        entry.flags=u16(central,at+8);entry.method=u16(central,at+10);entry.crc=u32(central,at+16);entry.compressed=u32(central,at+20);entry.visible.bytes=u32(central,at+24);entry.offset=u32(central,at+42);
        need(entry.compressed!=UINT32_MAX&&entry.visible.bytes!=UINT32_MAX&&entry.offset<central_offset&&entry.compressed<=central_offset-entry.offset);
        entry.visible.directory=entry.visible.name.back()=='/'||(u32(central,at+38)&16)!=0;
        entry.visible.encrypted=(entry.flags&0x2041)!=0;result.encrypted=result.encrypted||entry.visible.encrypted;
        const auto unix_type=(u32(central,at+38)>>16)&0170000U;
        entry.visible.readable=!entry.visible.directory&&!entry.visible.encrypted&&(unix_type==0||unix_type==0100000U)&&(entry.method==0||entry.method==8)&&(entry.flags&~0x080eU)==0;
        entry.central=central.substr(at,size);result.entries.push_back(std::move(entry));at+=size;
    }
    need(at==central.size());return result;
}
struct ArchiveFree{void operator()(struct archive* a)const{if(a)archive_read_free(a);}};
std::string contents(Reader& source,const Directory& directory,std::size_t index,std::size_t maximum){
    need(index<directory.entries.size()&&maximum>0&&maximum<=262144,Error::invalid_request,"Invalid ZIP view selection.");const auto& entry=directory.entries[index];
    need(entry.visible.readable,Error::archive_invalid,"Encrypted, linked or unsupported entry cannot be viewed.");
    need(entry.visible.bytes<=maximum&&entry.compressed<=1048576,Error::limit_exceeded,"ZIP member is too large to preview.");
    auto local=source.read(entry.offset,30);need(local.substr(0,4)==std::string("PK\3\4",4)&&u16(local,6)==entry.flags&&u16(local,8)==entry.method);
    const auto name=u16(local,26),extra=u16(local,28);const auto suffix=source.read(static_cast<std::uint64_t>(entry.offset)+30,static_cast<std::size_t>(name)+extra);
    need(suffix.substr(0,name)==entry.visible.name);local+=suffix;
    auto data_end=static_cast<std::uint64_t>(entry.offset)+local.size()+entry.compressed;need(data_end<=directory.offset);
    local+=source.read(static_cast<std::uint64_t>(entry.offset)+local.size(),entry.compressed);
    if(entry.flags&8){
        auto descriptor=source.read(data_end,12);if(descriptor.substr(0,4)==std::string("PK\7\10",4)){descriptor+=source.read(data_end+12,4);need(u32(descriptor,4)==entry.crc&&u32(descriptor,8)==entry.compressed&&u32(descriptor,12)==entry.visible.bytes);}
        else need(u32(descriptor,0)==entry.crc&&u32(descriptor,4)==entry.compressed&&u32(descriptor,8)==entry.visible.bytes);
        data_end+=descriptor.size();need(data_end<=directory.offset);local+=descriptor;
    }else need(u32(local,14)==entry.crc&&u32(local,18)==entry.compressed&&u32(local,22)==entry.visible.bytes);
    // Construct a bounded one-member memory envelope from verified original
    // ranges. This never rewrites the source or extracts any filesystem path.
    auto central=entry.central;put32(central,42,0);std::string end(22,'\0');end.replace(0,4,std::string("PK\5\6",4));put16(end,8,1);put16(end,10,1);put32(end,12,static_cast<std::uint32_t>(central.size()));put32(end,16,static_cast<std::uint32_t>(local.size()));
    const auto zip=local+central+end;std::unique_ptr<struct archive,ArchiveFree> archive(archive_read_new());
    need(archive&&archive_read_support_format_zip(archive.get())==ARCHIVE_OK&&archive_read_support_filter_none(archive.get())==ARCHIVE_OK&&archive_read_open_memory(archive.get(),zip.data(),zip.size())==ARCHIVE_OK);
    struct archive_entry* native{};need(archive_read_next_header(archive.get(),&native)==ARCHIVE_OK&&native&&archive_entry_filetype(native)==AE_IFREG&&archive_entry_is_encrypted(native)==0&&!archive_entry_symlink(native)&&!archive_entry_hardlink(native));
    std::string result;std::array<char,4096> buffer{};
    for(;;){source.cancel();const auto got=archive_read_data(archive.get(),buffer.data(),buffer.size());need(got>=0,Error::archive_invalid,"ZIP data or CRC verification failed.");if(!got)break;need(static_cast<std::size_t>(got)<=maximum-result.size(),Error::limit_exceeded,"Actual ZIP expansion exceeds preview limit.");result.append(buffer.data(),static_cast<std::size_t>(got));}
    need(result.size()==entry.visible.bytes&&archive_read_next_header(archive.get(),&native)==ARCHIVE_EOF&&archive_read_close(archive.get())==ARCHIVE_OK);return result;
}
Result failure(){return {Error::source_io,"ZIP source could not be read safely."};}
} // namespace
FileZipInspection inspect_file_zip(const FileZipSource& source){try{Reader reader{source};const auto directory=inspect(reader);FileZipInspection r;r.encrypted=directory.encrypted;for(const auto& entry:directory.entries)r.entries.push_back(entry.visible);return r;}catch(const Failure& f){return {{f.code,f.detail},{},false};}catch(...){return {failure(),{},false};}}
FileZipText read_file_zip_entry(const FileZipSource& source,std::size_t index,std::size_t maximum){try{Reader reader{source};const auto directory=inspect(reader);return {{},contents(reader,directory,index,maximum)};}catch(const Failure& f){return {{f.code,f.detail},{}};}catch(...){return {failure(),{}};}}
FileDizResult read_file_diz(const FileZipSource& source){try{
    Reader reader{source};const auto directory=inspect(reader);FileDizResult result;result.encrypted=directory.encrypted;
    if(result.encrypted){result.result={Error::archive_invalid,"Encrypted ZIP requires Sysop review."};return result;}
    for(std::size_t n=0;n<directory.entries.size();++n)if(folded(directory.entries[n].visible.name)=="FILE_ID.DIZ"){
        const auto text=contents(reader,directory,n,application::kFileLongDescriptionBytes);result.descriptions=application::file_diz_descriptions(text);break;
    }
    return result;
}catch(const Failure& f){return {{f.code,f.detail},{},false};}catch(...){return {failure(),{},false};}}
} // namespace sfx::archive
